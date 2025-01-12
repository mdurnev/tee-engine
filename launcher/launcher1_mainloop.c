/*****************************************************************************
** Copyright (C) 2013 Brian McGillion                                       **
**                                                                          **
** Licensed under the Apache License, Version 2.0 (the "License");          **
** you may not use this file except in compliance with the License.         **
** You may obtain a copy of the License at                                  **
**                                                                          **
**      http://www.apache.org/licenses/LICENSE-2.0                          **
**                                                                          **
** Unless required by applicable law or agreed to in writing, software      **
** distributed under the License is distributed on an "AS IS" BASIS,        **
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. **
** See the License for the specific language governing permissions and      **
** limitations under the License.                                           **
*****************************************************************************/

/* The launchers, sole purpose is to listen for commands from the manager.
 * When it receives a command from the manager it creates a new socket pair
 * and forks off a child process.  this child process will become a TA.
 * Once the child process is forked off, the launcher sends one end
 * of the newly created socket pair back to the manager so it can
 * communicate directly with the TA. The launcher then returns to wait until
 * it is required to start the next TA.
 *
 * In the child process the launcher loads the TA as a library and waits for
 * an open_session request to arrive from the manager so it cna complete its
 * initialization
 */

#define _GNU_SOURCE

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>

#include "subprocess.h"
#include "ta_process.h"
#include "ta_exit_states.h"
#include "core_control_resources.h"
#include "com_protocol.h"
#include "epoll_wrapper.h"
#include "tee_logging.h"

/* Child stack for clone command */
char *child_stack;

#define MAX_CURR_EVENTS 5
#define CHILD_STACK_SIZE 8192 /* this is a small stack, normal stack size is 8192 * 1024 */

#ifdef GRACEFUL_TERMINATION
/* Freeing only resources that are allocated here */
static void cleanup_launcher()
{
	cleanup_epoll();
	free(child_stack);
}
#endif

static void check_signal_status(struct core_control *control_params)
{
	sig_atomic_t cpy_sig_vec = control_params->sig_vector;
	control_params->reset_signal_self_pipe();

	/* Note: SIGPIPE and SIGGHLD is not handeled. SIGPIPE is handeled locally and
	 * launcher is not parenting any process. Launcher spwan new process, but it will
	 * transfer ownership to manager process and all child-status-change signals are
	 * delivered to manger process */

	if (cpy_sig_vec & TEE_SIG_TERM)
		exit(EXIT_SUCCESS);

	if (cpy_sig_vec & TEE_SIG_HUP) {
		/* At the moment, do nothing */
	}

	if (cpy_sig_vec & TEE_SIG_INT)
		exit(EXIT_SUCCESS);
}

static void send_err_msg_to_manager(int man_fd, struct com_msg_ta_created *msg)
{
	/* No special error message. PID -1 is signaling error */
	msg->pid = -1; /* TA not created */

	if (com_send_msg(man_fd, msg, sizeof(struct com_msg_ta_created), NULL, 0) !=
	    sizeof(struct com_msg_ta_created)) {
		OT_LOG(LOG_ERR, "Failed report fail");
	}
}

int lib_main_loop(struct core_control *ctl_params)
{
	int sockfd[2];
	struct com_msg_open_session *recv_open_msg = NULL;
	struct com_msg_ta_created new_ta_info;
	int ret, event_count, i;
	sigset_t sig_empty_set, sig_block_set;
	struct epoll_event cur_events[MAX_CURR_EVENTS];
	struct ta_loop_arg ta_loop_args = {0};
	int shm_fds[4];
	int shm_fd_count;

	memset(&new_ta_info, 0, sizeof(struct com_msg_ta_created));

	child_stack = calloc(1, CHILD_STACK_SIZE);
	if (!child_stack) {
		OT_LOG(LOG_ERR, "Sigempty set failed");
		exit(EXIT_FAILURE);
	}

	if (sigemptyset(&sig_empty_set)) {
		OT_LOG(LOG_ERR, "Sigempty set failed");
		exit(EXIT_FAILURE);
	}

	if (sigfillset(&sig_block_set)) {
		OT_LOG(LOG_ERR, "Sigempty set failed");
		exit(EXIT_FAILURE);
	}

	if (init_epoll()) {
		OT_LOG(LOG_ERR, "Epoll init failure");
		exit(EXIT_FAILURE);
	}

	/* listen to inbound connections from the manager */
	if (epoll_reg_fd(ctl_params->comm_sock_fd, EPOLLIN)) {
		OT_LOG(LOG_ERR, "Failed reg manager socket");
		exit(EXIT_FAILURE);
	}

	if (epoll_reg_fd(ctl_params->self_pipe_fd, EPOLLIN)) {
		OT_LOG(LOG_ERR, "Failed reg self pipe socket");
		exit(EXIT_FAILURE);
	}

#ifdef GRACEFUL_TERMINATION
	/* Cleanup function if process need to be cleaned gracefully */
	ctl_params->fn_cleanup_launher = cleanup_launcher;
#endif

	OT_LOG(LOG_ERR, "Entering the launcher mainloop");

	for (;;) {
		if (pthread_sigmask(SIG_SETMASK, &sig_empty_set, NULL)) {
			OT_LOG(LOG_ERR, "Problem with signal mask setting");
			continue;
		}

		event_count = wrap_epoll_wait(cur_events, MAX_CURR_EVENTS);
		if (event_count == -1) {
			if (errno == EINTR) {
				check_signal_status(ctl_params);
				continue;
			}

			/* Log error and hope the error clears itself */
			OT_LOG(LOG_ERR, "Failed return from epoll_wait");
			continue;
		}

		if (pthread_sigmask(SIG_SETMASK, &sig_block_set, NULL)) {
			OT_LOG(LOG_ERR, "Problem with signal mask setting");
			continue;
		}

		/* Note: All signals are blocked */

		for (i = 0; i < event_count; i++) {

			if (cur_events[i].data.fd == ctl_params->self_pipe_fd) {

				if (cur_events[i].events & EPOLLERR) {
					OT_LOG(LOG_ERR, "Something wrong with self pipe");
					exit(EXIT_FAILURE);
				}

				check_signal_status(ctl_params);
				continue;
			}

			/* Launcher is monitoring only two socket and second one is manager fd */
			if (cur_events[i].events & EPOLLERR || cur_events[i].events & EPOLLHUP) {
				OT_LOG(LOG_ERR, "Manager socket error");
				exit(EXIT_FAILURE);
			}

			ret = com_recv_msg(ctl_params->comm_sock_fd, (void **)&recv_open_msg, NULL,
					   shm_fds, &shm_fd_count);

			if (ret == -1) {
				free(recv_open_msg);
				/* TODO: Figur out why -1, but for now lets
				 *  hope the error clears itself*/
				continue;

			} else if (ret > 0) {
				/* ignore message */
				free(recv_open_msg);
				continue;
			}

			recv_open_msg->msg_hdr.shareable_fd_count = 0;
			if (shm_fd_count > 0 && shm_fd_count <= 4) {
				recv_open_msg->msg_hdr.shareable_fd_count = shm_fd_count;
				memcpy(recv_open_msg->msg_hdr.shareable_fd, shm_fds,
				       sizeof(int)*shm_fd_count);
			}

			/* Extrac info from message */
			if (recv_open_msg->msg_hdr.msg_name != COM_MSG_NAME_OPEN_SESSION ||
			    recv_open_msg->msg_hdr.msg_type != COM_TYPE_QUERY) {
				OT_LOG(LOG_ERR, "Invalid message");
				goto close_fd;
			}

			/* Received correct mesage from manager. Prepare response message.
			 * PID is filled later */
			new_ta_info.msg_hdr.msg_name = COM_MSG_NAME_CREATED_TA;
			new_ta_info.msg_hdr.msg_type = COM_TYPE_RESPONSE;
			new_ta_info.msg_hdr.sess_id = recv_open_msg->msg_hdr.sess_id;

			/* create a socket pair so the manager and TA can communicate */
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfd) == -1) {
				OT_LOG(LOG_ERR, "failed to create a socket pair");
				send_err_msg_to_manager(ctl_params->comm_sock_fd, &new_ta_info);
				goto close_fd;
			}

			/*
			 * Clone now to create the TA subprocess
			 */

			/* Fill ta loop arguments */
			ta_loop_args.com_sock = sockfd[1];
			ta_loop_args.ctl_params = ctl_params;
			ta_loop_args.recv_open_msg = recv_open_msg;

			new_ta_info.pid = getpid();

			ret = com_send_msg(ctl_params->comm_sock_fd, &new_ta_info,
					   sizeof(struct com_msg_ta_created), NULL, 0);

			if (ret == sizeof(struct com_msg_ta_created)) {

				if (send_fd(ctl_params->comm_sock_fd, &sockfd[0], 1, NULL, 0)
				    == -1) {
					OT_LOG(LOG_ERR, "Failed to send TA sock");
					/* TODO: Check what is causing error, but for now
						 * lets hope the error clears itself*/
				}
				else
					ta_process_loop(&ta_loop_args);

			} else {
				OT_LOG(LOG_ERR, "Failed to send response msg");
				/* TODO: Check what is causing error, but for now lets
					 *  hope the error clears itself*/
			}

			/* parent process will stay as the launcher */
			close(sockfd[0]);
			close(sockfd[1]);
close_fd:
			/* close possibly forwarded file descriptors */
			while (recv_open_msg->msg_hdr.shareable_fd_count > 0) {
				recv_open_msg->msg_hdr.shareable_fd_count--;
				close(recv_open_msg->msg_hdr.shareable_fd
						[recv_open_msg->msg_hdr.shareable_fd_count]);
			}
			free(recv_open_msg);

		}
	}
}
