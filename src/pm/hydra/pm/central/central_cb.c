/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 *  (C) 2008 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "hydra.h"
#include "hydra_utils.h"
#include "pmcu_pmi.h"
#include "pmci.h"
#include "bsci.h"
#include "demux.h"
#include "central.h"

int HYD_PMCD_Central_listenfd;
HYD_Handle handle;

/*
 * HYD_PMCD_Central_cb: This is the core PMI server part of the
 * code. Here's the expected protocol:
 *
 * 1. The client (MPI process) connects to us, which will result in an
 * event on the listener socket.
 *
 * 2. The client sends us a "cmd" or "mcmd" string which means that a
 * single or multi-line command is to follow.
 *
 * 3. Here are the commands that we respect:
 *     - initack [done]
 *     - init [done]
 *     - get_maxes [done]
 *     - get_appnum [done]
 *     - get_my_kvsname [done]
 *     - barrier_in [done]
 *     - put [done]
 *     - get [done]
 *     - finalize [done]
 *     - get_universe_size [done]
 *     - abort
 *     - create_kvs
 *     - destroy_kvs
 *     - getbyidx
 *     - spawn
 */
HYD_Status HYD_PMCD_Central_cb(int fd, HYD_Event_t events)
{
    int accept_fd, linelen, i;
    char *buf, *cmd, *args[HYD_PMCU_NUM_STR];
    HYD_Status status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    HYDU_MALLOC(buf, char *, HYD_TMPBUF_SIZE, status);

    if (fd == HYD_PMCD_Central_listenfd) {      /* Someone is trying to connect to us */
        status = HYDU_sock_accept(fd, &accept_fd);
        HYDU_ERR_POP(status, "accept error\n");

        status = HYD_DMX_register_fd(1, &accept_fd, HYD_STDOUT, HYD_PMCD_Central_cb);
        HYDU_ERR_POP(status, "unable to register fd\n");
    }
    else {
        status = HYDU_sock_readline(fd, buf, HYD_TMPBUF_SIZE, &linelen);
        HYDU_ERR_POP(status, "PMI read line error\n");

        if (linelen == 0) {
            /* This is not a clean close. If a finalize was called, we
             * would have deregistered this socket. The application
             * might have aborted. Just cleanup all the processes */
            status = HYD_PMCD_Central_cleanup();
            if (status != HYD_SUCCESS) {
                HYDU_Warn_printf("bootstrap server returned error cleaning up processes\n");
                status = HYD_SUCCESS;
                goto fn_fail;
            }

            status = HYD_DMX_deregister_fd(fd);
            if (status != HYD_SUCCESS) {
                HYDU_Warn_printf("unable to deregister fd %d\n", fd);
                status = HYD_SUCCESS;
                goto fn_fail;
            }

            close(fd);
            goto fn_exit;
        }

        /* Check what command we got and call the appropriate
         * function */
        buf[linelen - 1] = 0;

        cmd = strtok(buf, " ");
        for (i = 0; i < HYD_PMCU_NUM_STR; i++) {
            args[i] = strtok(NULL, " ");
            if (args[i] == NULL)
                break;
        }

        if (cmd == NULL) {
            status = HYD_SUCCESS;
        }
        else if (!strcmp("cmd=initack", cmd)) {
            status = HYD_PMCU_pmi_initack(fd, args);
        }
        else if (!strcmp("cmd=init", cmd)) {
            status = HYD_PMCU_pmi_init(fd, args);
        }
        else if (!strcmp("cmd=get_maxes", cmd)) {
            status = HYD_PMCU_pmi_get_maxes(fd, args);
        }
        else if (!strcmp("cmd=get_appnum", cmd)) {
            status = HYD_PMCU_pmi_get_appnum(fd, args);
        }
        else if (!strcmp("cmd=get_my_kvsname", cmd)) {
            status = HYD_PMCU_pmi_get_my_kvsname(fd, args);
        }
        else if (!strcmp("cmd=barrier_in", cmd)) {
            status = HYD_PMCU_pmi_barrier_in(fd, args);
        }
        else if (!strcmp("cmd=put", cmd)) {
            status = HYD_PMCU_pmi_put(fd, args);
        }
        else if (!strcmp("cmd=get", cmd)) {
            status = HYD_PMCU_pmi_get(fd, args);
        }
        else if (!strcmp("cmd=finalize", cmd)) {
            status = HYD_PMCU_pmi_finalize(fd, args);

            if (status == HYD_SUCCESS) {
                status = HYD_DMX_deregister_fd(fd);
                HYDU_ERR_POP(status, "unable to register fd\n");
                close(fd);
            }
        }
        else if (!strcmp("cmd=get_universe_size", cmd)) {
            status = HYD_PMCU_pmi_get_usize(fd, args);
        }
        else {
            /* We don't understand the command */
            HYDU_Error_printf("Unrecognized PMI command: %s; cleaning up processes\n", cmd);

            /* Cleanup all the processes and return. We don't need to
             * check the return status since we are anyway returning
             * an error */
            HYD_PMCD_Central_cleanup();
            status = HYD_SUCCESS;
            goto fn_fail;
        }

        HYDU_ERR_POP(status, "PMI server returned error\n");
    }

  fn_exit:
    HYDU_FREE(buf);
    HYDU_FUNC_EXIT();
    return status;

  fn_fail:
    goto fn_exit;
}


HYD_Status HYD_PMCD_Central_cleanup(void)
{
    struct HYD_Proc_params *proc_params;
    struct HYD_Partition_list *partition;
    int fd;
    enum HYD_Proxy_cmds cmd;
    HYD_Status status = HYD_SUCCESS, overall_status = HYD_SUCCESS;

    HYDU_FUNC_ENTER();

    /* FIXME: Instead of doing this from this process itself, fork a
     * bunch of processes to do this. */
    /* Connect to all proxies and send a KILL command */
    cmd = KILLALL_PROCS;
    for (proc_params = handle.proc_params; proc_params; proc_params = proc_params->next) {
        for (partition = proc_params->partition; partition; partition = partition->next) {
            status = HYDU_sock_connect(partition->name, handle.proxy_port, &fd);
            if (status != HYD_SUCCESS) {
                HYDU_Warn_printf("unable to connect to the proxy on %s\n", partition->name);
                overall_status = HYD_INTERNAL_ERROR;
                continue;       /* Move on to the next proxy */
            }

            status = HYDU_sock_write(fd, &cmd, sizeof(cmd));
            if (status != HYD_SUCCESS) {
                HYDU_Warn_printf("unable to send data to the proxy on %s\n", partition->name);
                overall_status = HYD_INTERNAL_ERROR;
                continue;       /* Move on to the next proxy */
            }

            close(fd);
        }
    }

    HYDU_FUNC_EXIT();

    return overall_status;
}


void HYD_PMCD_Central_signal_cb(int signal)
{
    HYDU_FUNC_ENTER();

    if (signal == SIGINT || signal == SIGQUIT || signal == SIGTERM
#if defined SIGSTOP
        || signal == SIGSTOP
#endif /* SIGSTOP */
#if defined SIGCONT
        || signal == SIGCONT
#endif /* SIGSTOP */
) {
        /* There's nothing we can do with the return value for now. */
        HYD_PMCD_Central_cleanup();
        exit(-1);
    }
    else {
        /* Ignore other signals for now */
    }

    HYDU_FUNC_EXIT();
    return;
}
