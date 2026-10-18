/*
 * Copyright (c) 2000-2020 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
/*
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */

#include <mach/mach_types.h>
#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/message.h>
#include <mach/port.h>
#include <mach/mig_errors.h>
#include <mach/task.h>
#include <mach/thread_status.h>
#include <mach/exception_types.h>
#include <mach/exc.h>
#include <mach/mach_exc.h>

#include <ipc/port.h>
#include <ipc/ipc_entry.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_notify.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_pset.h>
#include <ipc/ipc_machdep.h>

#include <kern/ipc_tt.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/host.h>
#include <kern/misc_protos.h>
#include <kern/ux_handler.h>
#include <kern/task_ident.h>

#include <vm/vm_map.h>

#include <security/mac_mach_internal.h>
#include <string.h>

#include <pexpert/pexpert.h>

#include <os/log.h>
#include <os/system_event_log.h>

#include <libkern/coreanalytics/coreanalytics.h>

#include <sys/code_signing.h> /* for developer mode state */

bool panic_on_exception_triage = false;

/* Not used in coded, only for inspection during debugging */
unsigned long c_thr_exc_raise = 0;
unsigned long c_thr_exc_raise_identity_token = 0;
unsigned long c_thr_exc_raise_state = 0;
unsigned long c_thr_exc_raise_state_id = 0;
unsigned long c_thr_exc_raise_backtrace = 0;

/* forward declarations */
kern_return_t exception_deliver(
	thread_t                thread,
	exception_type_t        exception,
	mach_exception_data_t   code,
	mach_msg_type_number_t  codeCnt,
	struct exception_action *excp,
	lck_mtx_t                       *mutex);

#ifdef MACH_BSD
kern_return_t bsd_exception(
	exception_type_t        exception,
	mach_exception_data_t   code,
	mach_msg_type_number_t  codeCnt);
#endif /* MACH_BSD */

#if __has_feature(ptrauth_calls)
extern int exit_with_pac_exception(
	void *proc,
	exception_type_t         exception,
	mach_exception_code_t    code,
	mach_exception_subcode_t subcode);
#endif /* __has_feature(ptrauth_calls) */

#ifdef MACH_BSD
extern bool proc_is_traced(void *p);
extern int      proc_selfpid(void);
extern char     *proc_name_address(struct proc *p);
#endif /* MACH_BSD */

#if (DEVELOPMENT || DEBUG)
TUNABLE_WRITEABLE(unsigned int, exception_log_max_pid, "exception_log_max_pid", 0);
#endif /* (DEVELOPMENT || DEBUG) */

/*
 * Routine: exception_init
 * Purpose:
 *   Global initialization of state for exceptions.
 * Conditions:
 *   None.
 */
void
exception_init(void)
{
	int tmp = 0;

	if (PE_parse_boot_argn("-panic_on_exception_triage", &tmp, sizeof(tmp))) {
		panic_on_exception_triage = true;
	}

#if (DEVELOPMENT || DEBUG)
	if (exception_log_max_pid) {
		printf("Logging all exceptions where pid < exception_log_max_pid (%d)\n", exception_log_max_pid);
	}
#endif /* (DEVELOPMENT || DEBUG) */
}

static TUNABLE(bool, pac_replace_ptrs_user, "pac_replace_ptrs_user", true);

ipc_port_t
exception_port_copy_send(ipc_port_t port)
{
	if (IP_VALID(port)) {
		if (is_ux_handler_port(port)) {
			/* is_ux_handler_port() compares against __DATA_CONST */
			port = ipc_port_copy_send_any(port);
		} else {
			port = ipc_port_copy_send_mqueue(port);
		}
	}
	return port;
}

/*
 *	Routine:	exception_deliver
 *	Purpose:
 *		Make an upcall to the exception server provided.
 *	Conditions:
 *		Nothing locked and no resources held.
 *		Called from an exception context, so
 *		thread_exception_return and thread_kdb_return
 *		are possible.
 *	Returns:
 *		KERN_SUCCESS if the exception was handled
 */
kern_return_t
exception_deliver(
	thread_t                thread,
	exception_type_t        exception,
	mach_exception_data_t   code,
	mach_msg_type_number_t  codeCnt,
	struct exception_action *excp,
	lck_mtx_t               *mutex)
{
	ipc_port_t              exc_port = IPC_PORT_NULL;
	exception_data_type_t   small_code[EXCEPTION_CODE_MAX];
	thread_state_t          new_state = NULL;
	int                     code64;
	int                     behavior;
	int                     flavor;
	kern_return_t           kr;
	task_t task;
	task_id_token_t task_token;
	ipc_port_t thread_port = IPC_PORT_NULL,
	    task_port = IPC_PORT_NULL,
	    task_token_port = IPC_PORT_NULL;

	/*
	 *  Save work if we are terminating.
	 *  Just go back to our AST handler.
	 */
	if (!thread->active && !thread->inspection) {
		return KERN_SUCCESS;
	}

	/*
	 * If there are no exception actions defined for this entity,
	 * we can't deliver here.
	 */
	if (excp == NULL) {
		return KERN_FAILURE;
	}

	assert(exception < EXC_TYPES_COUNT);
	if (exception >= EXC_TYPES_COUNT) {
		return KERN_FAILURE;
	}

	excp = &excp[exception];

	/*
	 * Snapshot the exception action data under lock for consistency.
	 * Hold a reference to the port over the exception_raise_* calls
	 * so it can't be destroyed.  This seems like overkill, but keeps
	 * the port from disappearing between now and when
	 * ipc_object_copyin_from_kernel is finally called.
	 */
	lck_mtx_lock(mutex);
	exc_port = exception_port_copy_send(excp->port);
	if (!IP_VALID(exc_port)) {
		lck_mtx_unlock(mutex);
		return KERN_FAILURE;
	}

	flavor = excp->flavor;
	behavior = excp->behavior;
	lck_mtx_unlock(mutex);

	code64 = (behavior & MACH_EXCEPTION_CODES);
	behavior &= ~MACH_EXCEPTION_MASK;

	if (!code64) {
		small_code[0] = CAST_DOWN_EXPLICIT(exception_data_type_t, code[0]);
		small_code[1] = CAST_DOWN_EXPLICIT(exception_data_type_t, code[1]);
	}

	task = get_threadtask(thread);

#if CONFIG_MACF
	/* Now is a reasonably good time to check if the exception action is
	 * permitted for this process, because after this point we will send
	 * the message out almost certainly.
	 * As with other failures, exception_triage_thread will go on
	 * to the next level.
	 */

	/* The global exception-to-signal translation port is safe to be an exception handler. */
	if (is_ux_handler_port(exc_port) == FALSE &&
	    mac_exc_action_check_exception_send(task, excp) != 0) {
		kr = KERN_FAILURE;
		goto out_release_right;
	}
#endif

	thread->options |= TH_IN_MACH_EXCEPTION;

	switch (behavior) {
	case EXCEPTION_STATE: {
		mach_msg_type_number_t old_state_cnt, new_state_cnt;
		thread_state_data_t old_state;
		thread_set_status_flags_t get_flags = TSSF_TRANSLATE_TO_USER;
		thread_set_status_flags_t set_flags = TSSF_CHECK_USER_FLAGS;
		bool task_allow_user_state = task_needs_user_signed_thread_state(task);

		if (pac_replace_ptrs_user || task_allow_user_state) {
			get_flags |= TSSF_RANDOM_USER_DIV;
			set_flags |= (TSSF_ALLOW_ONLY_USER_PTRS | TSSF_RANDOM_USER_DIV);
		}

		c_thr_exc_raise_state++;
		old_state_cnt = _MachineStateCount[flavor];
		kr = thread_getstatus_to_user(thread, flavor,
		    (thread_state_t)old_state,
		    &old_state_cnt, get_flags);
		new_state_cnt = old_state_cnt;
		if (kr == KERN_SUCCESS) {
			new_state = (thread_state_t)kalloc_data(sizeof(thread_state_data_t), Z_WAITOK | Z_ZERO);
			if (new_state == NULL) {
				kr = KERN_RESOURCE_SHORTAGE;
				goto out_release_right;
			}
			if (code64) {
				kr = mach_exception_raise_state(exc_port,
				    exception,
				    code,
				    codeCnt,
				    &flavor,
				    old_state, old_state_cnt,
				    new_state, &new_state_cnt);
			} else {
				kr = exception_raise_state(exc_port, exception,
				    small_code,
				    codeCnt,
				    &flavor,
				    old_state, old_state_cnt,
				    new_state, &new_state_cnt);
			}
			if (kr == KERN_SUCCESS) {
				if (exception != EXC_CORPSE_NOTIFY) {
					kr = thread_setstatus_from_user(thread, flavor,
					    (thread_state_t)new_state, new_state_cnt,
					    (thread_state_t)old_state, old_state_cnt,
					    set_flags);
				}
				goto out_release_right;
			}
		}

		goto out_release_right;
	}

	case EXCEPTION_DEFAULT: {
		c_thr_exc_raise++;

		task_reference(task);
		thread_reference(thread);
		/*
		 * Only deliver control port if Developer Mode enabled,
		 * or task is a corpse. Otherwise we only deliver the
		 * (immovable) read port in exception handler (both in
		 * or out of process). (94669540)
		 */
		if (developer_mode_state() || task_is_a_corpse(task)) {
			task_port = convert_task_to_port(task);
			thread_port = convert_thread_to_port(thread);
		} else {
			task_port = convert_task_read_to_port(task);
			thread_port = convert_thread_read_to_port(thread);
		}
		/* task and thread ref consumed */

		if (code64) {
			kr = mach_exception_raise(exc_port,
			    thread_port,
			    task_port,
			    exception,
			    code,
			    codeCnt);
		} else {
			kr = exception_raise(exc_port,
			    thread_port,
			    task_port,
			    exception,
			    small_code,
			    codeCnt);
		}

		goto out_release_right;
	}

	case EXCEPTION_IDENTITY_PROTECTED: {
		c_thr_exc_raise_identity_token++;

		kr = task_create_identity_token(task, &task_token);
		/* task_token now represents a task, or corpse */
		assert(kr == KERN_SUCCESS);
		task_token_port = convert_task_id_token_to_port(task_token);
		/* task token ref consumed */

		if (code64) {
			kr = mach_exception_raise_identity_protected(exc_port,
			    thread->thread_id,
			    task_token_port,
			    exception,
			    code,
			    codeCnt);
		} else {
			panic("mach_exception_raise_identity_protected() must be code64");
		}

		goto out_release_right;
	}

	case EXCEPTION_STATE_IDENTITY: {
		mach_msg_type_number_t old_state_cnt, new_state_cnt;
		thread_state_data_t old_state;
		thread_set_status_flags_t get_flags = TSSF_TRANSLATE_TO_USER;
		thread_set_status_flags_t set_flags = TSSF_CHECK_USER_FLAGS;
		bool task_allow_user_state = task_needs_user_signed_thread_state(task);

		if (pac_replace_ptrs_user || task_allow_user_state) {
			get_flags |= TSSF_RANDOM_USER_DIV;
			set_flags |= (TSSF_ALLOW_ONLY_USER_PTRS | TSSF_RANDOM_USER_DIV);
		}

		c_thr_exc_raise_state_id++;

		task_reference(task);
		thread_reference(thread);
		/*
		 * Only deliver control port if Developer Mode enabled,
		 * or task is a corpse. Otherwise we only deliver the
		 * (immovable) read port in exception handler (both in
		 * or out of process). (94669540)
		 */
		if (developer_mode_state() || task_is_a_corpse(task)) {
			task_port = convert_task_to_port(task);
			thread_port = convert_thread_to_port(thread);
		} else {
			task_port = convert_task_read_to_port(task);
			thread_port = convert_thread_read_to_port(thread);
		}
		/* task and thread ref consumed */

		old_state_cnt = _MachineStateCount[flavor];
		kr = thread_getstatus_to_user(thread, flavor,
		    (thread_state_t)old_state,
		    &old_state_cnt, get_flags);
		new_state_cnt = old_state_cnt;
		if (kr == KERN_SUCCESS) {
			new_state = (thread_state_t)kalloc_data(sizeof(thread_state_data_t), Z_WAITOK | Z_ZERO);
			if (new_state == NULL) {
				kr = KERN_RESOURCE_SHORTAGE;
				goto out_release_right;
			}
			if (code64) {
				kr = mach_exception_raise_state_identity(
					exc_port,
					thread_port,
					task_port,
					exception,
					code,
					codeCnt,
					&flavor,
					old_state, old_state_cnt,
					new_state, &new_state_cnt);
			} else {
				kr = exception_raise_state_identity(exc_port,
				    thread_port,
				    task_port,
				    exception,
				    small_code,
				    codeCnt,
				    &flavor,
				    old_state, old_state_cnt,
				    new_state, &new_state_cnt);
			}

			if (kr == KERN_SUCCESS) {
				if (exception != EXC_CORPSE_NOTIFY &&
				    ip_kotype(thread_port) == IKOT_THREAD_CONTROL) {
					kr = thread_setstatus_from_user(thread, flavor,
					    (thread_state_t)new_state, new_state_cnt,
					    (thread_state_t)old_state, old_state_cnt, set_flags);
				}
				goto out_release_right;
			}
		}

		goto out_release_right;
	}

	default:
		panic("bad exception behavior!");
		return KERN_FAILURE;
	}/* switch */

out_release_right:

	thread->options &= ~TH_IN_MACH_EXCEPTION;

	if (task_port) {
		ipc_port_release_send(task_port);
	}

	if (thread_port) {
		ipc_port_release_send(thread_port);
	}

	if (exc_port) {
		ipc_port_release_send(exc_port);
	}

	if (task_token_port) {
		ipc_port_release_send(task_token_port);
	}

	if (new_state) {
		kfree_data(new_state, sizeof(thread_state_data_t));
	}

	return kr;
}

/*
 * Attempt exception delivery with backtrace info to exception ports
 * in exc_ports in order.
 */
/*
 *	Routine:	exception_deliver_backtrace
 *	Purpose:
 *      Attempt exception delivery with backtrace info to exception ports
 *      in exc_ports in order.
 *	Conditions:
 *		Caller has a reference on bt_object, and send rights on exc_ports.
 *		Does not consume any passed references or rights
 */
void
exception_deliver_backtrace(
	kcdata_object_t  bt_object,
	ipc_port_t       exc_ports[static BT_EXC_PORTS_COUNT],
	exception_type_t exception)
{
	kern_return_t kr;
	mach_exception_data_type_t code[EXCEPTION_CODE_MAX];
	ipc_port_t target_port, bt_obj_port;

	assert(exception == EXC_GUARD);

	code[0] = exception;
	code[1] = 0;

	kcdata_object_reference(bt_object);
	bt_obj_port = convert_kcdata_object_to_port(bt_object);
	/* backtrace object ref consumed, no-senders is armed */

	if (!IP_VALID(bt_obj_port)) {
		return;
	}

	/*
	 * We are guaranteed at task_enqueue_exception_with_corpse() time
	 * that the exception port prefers backtrace delivery.
	 */
	for (unsigned int i = 0; i < BT_EXC_PORTS_COUNT; i++) {
		target_port = exc_ports[i];

		if (!IP_VALID(target_port)) {
			continue;
		}

		ip_mq_lock(target_port);
		if (!ip_active(target_port)) {
			ip_mq_unlock(target_port);
			continue;
		}
		ip_mq_unlock(target_port);

		kr = mach_exception_raise_backtrace(target_port,
		    bt_obj_port,
		    EXC_CORPSE_NOTIFY,
		    code,
		    EXCEPTION_CODE_MAX);

		if (kr == KERN_SUCCESS || kr == MACH_RCV_PORT_DIED) {
			/* Exception is handled at this level */
			break;
		}
	}

	/* May trigger no-senders notification for backtrace object */
	ipc_port_release_send(bt_obj_port);

	return;
}

/*
 * Routine: check_exc_receiver_dependency
 * Purpose:
 *      Verify that the port destined for receiving this exception is not
 *      on the current task. This would cause hang in kernel for
 *      EXC_CRASH primarily. Note: If port is transferred
 *      between check and delivery then deadlock may happen.
 *
 * Conditions:
 *		Nothing locked and no resources held.
 *		Called from an exception context.
 * Returns:
 *      KERN_SUCCESS if its ok to send exception message.
 */
static kern_return_t
check_exc_receiver_dependency(
	exception_type_t exception,
	struct exception_action *excp,
	lck_mtx_t *mutex)
{
	kern_return_t retval = KERN_SUCCESS;

	if (excp == NULL || exception != EXC_CRASH) {
		return retval;
	}

	task_t task = current_task();
	lck_mtx_lock(mutex);
	ipc_port_t xport = excp[exception].port;
	if (IP_VALID(xport) && ip_in_space_noauth(xport, task->itk_space)) {
		retval = KERN_FAILURE;
	}
	lck_mtx_unlock(mutex);
	return retval;
}


/*
 *	Routine:	exception_triage_thread
 *	Purpose:
 *		The thread caught an exception.
 *		We make an up-call to the thread's exception server.
 *	Conditions:
 *		Nothing locked and no resources held.
 *		Called from an exception context, so
 *		thread_exception_return and thread_kdb_return
 *		are possible.
 *	Returns:
 *		KERN_SUCCESS if exception is handled by any of the handlers.
 */
kern_return_t
exception_triage_thread(
	exception_type_t        exception,
	mach_exception_data_t   code,
	mach_msg_type_number_t  codeCnt,
	thread_t                thread)
{
	task_t                  task;
	thread_ro_t             tro;
	host_priv_t             host_priv;
	lck_mtx_t               *mutex;
	struct exception_action *actions;
	kern_return_t   kr = KERN_FAILURE;

	assert(exception != EXC_RPC_ALERT);

	/*
	 * If this behavior has been requested by the the kernel
	 * (due to the boot environment), we should panic if we
	 * enter this function.  This is intended as a debugging
	 * aid; it should allow us to debug why we caught an
	 * exception in environments where debugging is especially
	 * difficult.
	 */
	if (panic_on_exception_triage) {
		panic("called exception_triage when it was forbidden by the boot environment");
	}

	/*
	 * Try to raise the exception at the activation level.
	 */
	mutex   = &thread->mutex;
	tro     = get_thread_ro(thread);
	actions = tro->tro_exc_actions;
	if (KERN_SUCCESS == check_exc_receiver_dependency(exception, actions, mutex)) {
		kr = exception_deliver(thread, exception, code, codeCnt, actions, mutex);
		if (kr == KERN_SUCCESS || kr == MACH_RCV_PORT_DIED) {
			goto out;
		}
	}

	/*
	 * Maybe the task level will handle it.
	 */
	task    = tro->tro_task;
	mutex   = &task->itk_lock_data;
	actions = task->exc_actions;
	if (KERN_SUCCESS == check_exc_receiver_dependency(exception, actions, mutex)) {
		kr = exception_deliver(thread, exception, code, codeCnt, actions, mutex);
		if (kr == KERN_SUCCESS || kr == MACH_RCV_PORT_DIED) {
			goto out;
		}
	}

	/*
	 * How about at the host level?
	 */
	host_priv = host_priv_self();
	mutex     = &host_priv->lock;
	actions   = host_priv->exc_actions;
	if (KERN_SUCCESS == check_exc_receiver_dependency(exception, actions, mutex)) {
		kr = exception_deliver(thread, exception, code, codeCnt, actions, mutex);
		if (kr == KERN_SUCCESS || kr == MACH_RCV_PORT_DIED) {
			goto out;
		}
	}

out:
	if ((exception != EXC_CRASH) && (exception != EXC_RESOURCE) &&
	    (exception != EXC_GUARD) && (exception != EXC_CORPSE_NOTIFY)) {
		thread_exception_return();
	}
	return kr;
}

#if __has_feature(ptrauth_calls)
static TUNABLE(bool, pac_exception_telemetry, "-pac_exception_telemetry", false);

CA_EVENT(pac_exception_event,
    CA_INT, exception,
    CA_INT, exception_code_0,
    CA_INT, exception_code_1,
    CA_STATIC_STRING(CA_PROCNAME_LEN), proc_name);

static void
pac_exception_triage(
	exception_type_t        exception,
	mach_exception_data_t   code)
{
	boolean_t traced_flag = FALSE;
	task_t task = current_task();
	void *proc = get_bsdtask_info(task);
	char *proc_name = (char *) "unknown";
	int pid = 0;

#ifdef MACH_BSD
	pid = proc_selfpid();
	if (proc) {
		traced_flag = proc_is_traced(proc);
		/* Should only be called on current proc */
		proc_name = proc_name_address(proc);

		/*
		 * For a ptrauth violation, check if process isn't being ptraced and
		 * the task has the TFRO_PAC_EXC_FATAL flag set. If both conditions are true,
		 * terminate the task via exit_with_reason
		 */
		if (!traced_flag) {
			if (pac_exception_telemetry) {
				ca_event_t ca_event = CA_EVENT_ALLOCATE(pac_exception_event);
				CA_EVENT_TYPE(pac_exception_event) * pexc_event = ca_event->data;
				pexc_event->exception = exception;
				pexc_event->exception_code_0 = code[0];
				pexc_event->exception_code_1 = code[1];
				strlcpy(pexc_event->proc_name, proc_name, CA_PROCNAME_LEN);
				CA_EVENT_SEND(ca_event);
			}
			if (task_is_pac_exception_fatal(task)) {
				os_log_error(OS_LOG_DEFAULT, "%s: process %s[%d] hit a pac violation\n", __func__, proc_name, pid);
				exit_with_pac_exception(proc, exception, code[0], code[1]);
				thread_exception_return();
				/* NOT_REACHABLE */
			}
		}
	}
#endif /* MACH_BSD */
}
#endif /* __has_feature(ptrauth_calls) */

/*
 *	Routine:	exception_triage
 *	Purpose:
 *		The current thread caught an exception.
 *		We make an up-call to the thread's exception server.
 *	Conditions:
 *		Nothing locked and no resources held.
 *		Called from an exception context, so
 *		thread_exception_return and thread_kdb_return
 *		are possible.
 *	Returns:
 *		KERN_SUCCESS if exception is handled by any of the handlers.
 */
int debug4k_panic_on_exception = 0;
kern_return_t
exception_triage(
	exception_type_t        exception,
	mach_exception_data_t   code,
	mach_msg_type_number_t  codeCnt)
{
	thread_t thread = current_thread();
	task_t   task   = current_task();

	assert(codeCnt > 0);

	if (VM_MAP_PAGE_SIZE(task->map) < PAGE_SIZE) {
		DEBUG4K_EXC("thread %p task %p map %p exception %d codes 0x%llx 0x%llx\n",
		    thread, task, task->map, exception, code[0], codeCnt > 1 ? code[1] : 0);
		if (debug4k_panic_on_exception) {
			panic("DEBUG4K thread %p task %p map %p exception %d codes 0x%llx 0x%llx",
			    thread, task, task->map, exception, code[0], codeCnt > 1 ? code[1] : 0);
		}
	}

#if (DEVELOPMENT || DEBUG)
#ifdef MACH_BSD
	if (proc_pid(get_bsdtask_info(task)) <= exception_log_max_pid) {
		record_system_event(SYSTEM_EVENT_TYPE_INFO, SYSTEM_EVENT_SUBSYSTEM_PROCESS, "process exit",
		    "exception_log_max_pid: pid %d (%s): sending exception %d (0x%llx 0x%llx)",
		    proc_pid(get_bsdtask_info(task)), proc_name_address(get_bsdtask_info(task)),
		    exception, code[0], codeCnt > 1 ? code[1] : 0);
	}
#endif /* MACH_BSD */
#endif /* DEVELOPMENT || DEBUG */

#if __has_feature(ptrauth_calls)
	if (exception & EXC_PTRAUTH_BIT) {
		exception &= ~EXC_PTRAUTH_BIT;
		assert(codeCnt == 2);
		pac_exception_triage(exception, code);
	}
#endif /* __has_feature(ptrauth_calls) */
	return exception_triage_thread(exception, code, codeCnt, thread);
}

#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <vm/vm_shared_region.h>
#include <arm/thread.h>

/*
 * SWEET crash report on the console. The phone has no way to get crash
 * logs off the device, and corpses stop after a few crashes, so print what
 * symbolication needs: user registers, a frame-pointer backtrace, the
 * shared cache slide (unslid = pc - slide) and the main binary's Mach-O
 * header (offset = pc - mh). Called from proc_prepareexit on the exiting
 * (crashing) thread.
 */
void sweet_crash_log(const char *name, int pid, int sig, uint64_t code, uint64_t subcode);
static void sweet_crash_strings(const char *name);

void
sweet_crash_log(const char *name, int pid, int sig, uint64_t code, uint64_t subcode)
{
	thread_t thread = current_thread();
	task_t task = current_task();
	arm_saved_state_t *ss = find_user_regs(thread);
	uint64_t slide = 0, mh = task->mach_header_vm_address;

	vm_shared_region_t sr = vm_shared_region_get(task);
	if (sr != NULL) {
		slide = sr->sr_slide;
		vm_shared_region_deallocate(sr);
	}
	printf("CRASH %s[%d] sig %d code 0x%llx sub 0x%llx slide 0x%llx mh 0x%llx\n",
	    name, pid, sig, code, subcode, slide, mh);
	if (ss == NULL || !is_saved_state64(ss)) {
		return;
	}
	uint64_t fp = get_saved_state_fp(ss);
	printf("CRASH %s pc 0x%llx lr 0x%llx sp 0x%llx fp 0x%llx far 0x%llx\n", name,
	    get_saved_state_pc(ss), get_saved_state_lr(ss), get_saved_state_sp(ss), fp,
	    (uint64_t)saved_state64(ss)->far);

	char line[200];
	int n = snprintf(line, sizeof(line), "CRASH %s bt", name);
	for (int i = 0; i < 10 && fp != 0 && (fp & 7) == 0 && n < (int)sizeof(line) - 20; i++) {
		uint64_t frame[2];
		if (copyin((user_addr_t)fp, frame, sizeof(frame)) != 0) {
			break;
		}
		n += snprintf(line + n, sizeof(line) - n, " 0x%llx", frame[1]);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
	printf("%s\n", line);

	if (sig == 11 || sig == 10) {
		/* Which mapping the faulting address is in, or which two it falls between. */
		uint64_t far = (uint64_t)saved_state64(ss)->far;
		vm_map_t map = current_map();
		vm_map_entry_t entry;
		vm_map_lock_read(map);
		bool inside = vm_map_lookup_entry(map, (vm_map_offset_t)far, &entry);
		for (int i = 0; i < 2 && entry != vm_map_to_entry(map); i++) {
			printf("CRASH %s map%s 0x%llx-0x%llx prot %d/%d tag %d\n", name,
			    inside && i == 0 ? " (far inside)" : (i == 0 ? " before far" : " after far"),
			    (uint64_t)entry->vme_start, (uint64_t)entry->vme_end, entry->protection,
			    entry->max_protection, (int)VME_ALIAS(entry));
			if (inside) {
				break;
			}
			entry = entry->vme_next;
		}
		vm_map_unlock_read(map);
	}
	if (sig == 6 || sig == 5) {
		sweet_crash_strings(name);
	}
}

/*
 * SWEET: for a process named in boot-arg sweetscan (comma separated,
 * default "backboardd,SpringBoard"; "-" = off) that dies with SIGABRT or
 * SIGTRAP (BSAssert / NSAssert style failures) print the printable text runs
 * (>= 24 characters, at least 2 spaces) found in its malloc heap as
 * "STR text", at most 700 lines. An uncaught NSException's reason string
 * is among them: the abort message only carries the exception's type.
 */
static void
sweet_crash_strings(const char *name)
{
	static char names[128];
	char *buf;
	vm_map_t map = current_map();
	vm_map_offset_t addr = 0;
	uint64_t scanned = 0;
	int lines = 0;
	char last[96] = "";

	if (!PE_parse_boot_argn("sweetscan", names, sizeof(names))) {
		strlcpy(names, "backboardd,SpringBoard", sizeof(names));
	}
	size_t n = strlen(name), found = 0;
	for (const char *t = names; *t;) {
		const char *end = strchr(t, ',');
		size_t tl = end ? (size_t)(end - t) : strlen(t);
		if (tl == n && n > 0 && strncmp(t, name, n) == 0) {
			found = 1;
			break;
		}
		if (!end) {
			break;
		}
		t = end + 1;
	}
	if (!found) {
		return;
	}
	buf = kalloc_data(65536, Z_WAITOK);
	if (buf == NULL) {
		return;
	}
	printf("STR scan of %s heap\n", name);
	while (lines < 700 && scanned < (96ull << 20)) {
		vm_map_size_t size = 0;
		natural_t depth = 0;
		vm_region_submap_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_V2_COUNT_64;

		if (vm_map_region_recurse_64(map, &addr, &size, &depth,
		    (vm_region_submap_info_64_t)&info, &count) != KERN_SUCCESS) {
			break;
		}
		vm_map_offset_t base = addr;
		addr += size;
		if (info.is_submap || (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) !=
		    (VM_PROT_READ | VM_PROT_WRITE) || info.user_tag < 1 || info.user_tag > 11) {
			continue;
		}
		for (vm_map_offset_t off = 0; off < size && lines < 700; off += 65536) {
			size_t len = (size - off) < 65536 ? (size_t)(size - off) : 65536;
			if (copyin((user_addr_t)(base + off), buf, len) != 0) {
				continue;
			}
			scanned += len;
			size_t start = 0;
			int spaces = 0;
			for (size_t i = 0; i <= len; i++) {
				bool ok = i < len && ((unsigned char)buf[i] >= 0x20 && (unsigned char)buf[i] < 0x7f);
				if (ok) {
					spaces += buf[i] == ' ';
					continue;
				}
				size_t run = i - start;
				if (run >= 24 && spaces >= 2) {
					char text[160];
					size_t m = run < sizeof(text) - 1 ? run : sizeof(text) - 1;
					memcpy(text, buf + start, m);
					text[m] = 0;
					if (strncmp(text, last, sizeof(last) - 1) != 0) {
						printf("STR %s\n", text);
						lines++;
						strlcpy(last, text, sizeof(last));
					}
				}
				start = i + 1;
				spaces = 0;
			}
		}
	}
	printf("STR done: %llu KiB scanned, %d strings\n", scanned >> 10, lines);
	kfree_data(buf, 65536);
}
/*
 * SWEET process sampler (bsd/dev/sweet_sample.c): user pc/lr/sp and a frame
 * pointer backtrace of every thread of a task, from the saved user state.
 */
void sweet_sample_task(const char *name, int pid, task_t task);

void
sweet_sample_task(const char *name, int pid, task_t task)
{
	vm_map_t map = get_task_map(task);
	uint64_t slide = 0;
	int tn = 0;
	thread_t thread;
	vm_shared_region_t sr = vm_shared_region_get(task);

	if (sr != NULL) {
		slide = sr->sr_slide;
		vm_shared_region_deallocate(sr);
	}
	printf("SMP %s[%d] slide 0x%llx mh 0x%llx\n", name, pid, slide, task->mach_header_vm_address);

	task_lock(task);
	queue_iterate(&task->threads, thread, thread_t, task_threads) {
		arm_saved_state_t *ss = find_user_regs(thread);
		char line[400];
		int n;
		uint64_t fp;

		if (ss == NULL || !is_saved_state64(ss)) {
			continue;
		}
		fp = get_saved_state_fp(ss);
		n = snprintf(line, sizeof(line), "SMP %s t%d pc 0x%llx lr 0x%llx sp 0x%llx bt", name, tn++,
		    get_saved_state_pc(ss), get_saved_state_lr(ss), get_saved_state_sp(ss));
		for (int i = 0; i < 16 && fp != 0 && (fp & 7) == 0 && n < (int)sizeof(line) - 20; i++) {
			uint64_t frame[2];
			if (vm_map_read_user(map, fp, frame, sizeof(frame)) != KERN_SUCCESS) {
				break;
			}
			n += snprintf(line + n, sizeof(line) - n, " 0x%llx", frame[1]);
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
		printf("%s\n", line);
	}
	task_unlock(task);
}

#endif /* ARM64_BOARD_CONFIG_SWEET */

kern_return_t
bsd_exception(
	exception_type_t        exception,
	mach_exception_data_t   code,
	mach_msg_type_number_t  codeCnt)
{
	task_t                  task;
	lck_mtx_t               *mutex;
	thread_t                self = current_thread();
	kern_return_t           kr;

	/*
	 * Maybe the task level will handle it.
	 */
	task = current_task();
	mutex = &task->itk_lock_data;

	kr = exception_deliver(self, exception, code, codeCnt, task->exc_actions, mutex);

	if (kr == KERN_SUCCESS || kr == MACH_RCV_PORT_DIED) {
		return KERN_SUCCESS;
	}
	return KERN_FAILURE;
}


/*
 * Raise an exception on a task.
 * This should tell launchd to launch Crash Reporter for this task.
 */
kern_return_t
task_exception_notify(exception_type_t exception,
    mach_exception_data_type_t exccode, mach_exception_data_type_t excsubcode)
{
	mach_exception_data_type_t      code[EXCEPTION_CODE_MAX];
	wait_interrupt_t                wsave;
	kern_return_t kr = KERN_SUCCESS;

	code[0] = exccode;
	code[1] = excsubcode;

	wsave = thread_interrupt_level(THREAD_UNINT);
	kr = exception_triage(exception, code, EXCEPTION_CODE_MAX);
	(void) thread_interrupt_level(wsave);
	return kr;
}


/*
 *	Handle interface for special performance monitoring
 *	This is a special case of the host exception handler
 */
kern_return_t
sys_perf_notify(thread_t thread, int pid)
{
	host_priv_t             hostp;
	ipc_port_t              xport;
	wait_interrupt_t        wsave;
	kern_return_t           ret;

	hostp = host_priv_self();       /* Get the host privileged ports */
	mach_exception_data_type_t      code[EXCEPTION_CODE_MAX];
	code[0] = 0xFF000001;           /* Set terminate code */
	code[1] = pid;          /* Pass out the pid */

	lck_mtx_lock(&hostp->lock);
	xport = hostp->exc_actions[EXC_RPC_ALERT].port;

	/* Make sure we're not catching our own exception */
	if (!IP_VALID(xport) ||
	    !ip_active(xport) ||
	    ip_in_space_noauth(xport, get_threadtask(thread)->itk_space)) {
		lck_mtx_unlock(&hostp->lock);
		return KERN_FAILURE;
	}

	lck_mtx_unlock(&hostp->lock);

	wsave = thread_interrupt_level(THREAD_UNINT);
	ret = exception_deliver(
		thread,
		EXC_RPC_ALERT,
		code,
		2,
		hostp->exc_actions,
		&hostp->lock);
	(void)thread_interrupt_level(wsave);

	return ret;
}
