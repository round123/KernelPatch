/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 
 * Copyright (C) 2023 bmax121. All Rights Reserved.
 */

#include <ktypes.h>
#include <uapi/scdefs.h>
#include <hook.h>
#include <common.h>
#include <log.h>
#include <predata.h>
#include <pgtable.h>
#include <linux/syscall.h>
#include <uapi/asm-generic/errno.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <asm/current.h>
#include <linux/string.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <syscall.h>
#include <accctl.h>
#include <module.h>
#include <kputils.h>
#include <linux/err.h>
#include <linux/slab.h>

#define MAX_KEY_LEN 128

#include <linux/umh.h>

static long call_test(long arg1, long arg2, long arg3)
{
    char *cmd = "/system/bin/touch";
    // const char *superkey = get_superkey();
    char *argv[] = {
        cmd,
        "/data/local/tmp/test.txt",
        NULL,
    };
    char *envp[] = {
        "PATH=/system/bin:/data/adb",
        NULL,
    };
    int rc = call_usermodehelper(cmd, argv, envp, UMH_WAIT_PROC);
    log_boot("user_init: %d\n", rc);
    return 0;
}

static long call_klog(const char __user *arg1)
{
    char buf[1024];
    long len = strncpy_from_user_nofault(buf, arg1, sizeof(buf));
    if (len <= 0) return -EINVAL;
    if (len > 0) logki("user log: %s", buf);
    return 0;
}

static long call_kpm_load(const char __user *arg1, const char *__user arg2)
{
    char path[512], args[512];
    long pathlen = strncpy_from_user_nofault(path, arg1, sizeof(path));
    if (pathlen <= 0) return -EINVAL;
    long arglen = strncpy_from_user_nofault(args, arg2, sizeof(args));
    return load_module_path(path, arglen <= 0 ? 0 : args);
}

static long call_kpm_unload(const char *__user arg1)
{
    char name[512];
    long len = strncpy_from_user_nofault(name, arg1, sizeof(name));
    if (len <= 0) return -EINVAL;
    return unload_module(name);
}

static long call_kpm_nums()
{
    return get_module_nums();
}

static long call_kpm_list(char *__user names, int len)
{
    if (len <= 0) return -EINVAL;
    char buf[4096];
    int sz = list_modules(buf, sizeof(buf));
    if (sz > len) return -ENOBUFS;
    sz = seq_copy_to_user(names, buf, len);
    if (sz < 0) return sz;
    return 0;
}

static long call_kpm_info(const char *__user uname, char *__user out_info, int out_len)
{
    if (out_len <= 0) return -EINVAL;
    char name[64];
    char buf[2048];
    int len = strncpy_from_user_nofault(name, uname, sizeof(name));
    if (len <= 0) return -EINVAL;
    int sz = get_module_info(name, buf, sizeof(buf));
    if (sz < 0) return sz;
    if (sz > out_len) return -ENOBUFS;
    sz = seq_copy_to_user(out_info, buf, sz);
    if (sz < 0) return sz;
    return 0;
}

static long call_su(struct su_profile *__user uprofile)
{
    struct su_profile *profile = memdup_user(uprofile, sizeof(struct su_profile));
    if (IS_ERR(profile)) return PTR_ERR(profile);
    profile->scontext[sizeof(profile->scontext) - 1] = '\0';
    int rc = commit_su(profile->to_uid, profile->scontext);
    kvfree(profile);
    return rc;
}

static long call_su_task(pid_t pid, struct su_profile *__user uprofile)
{
    struct su_profile *profile = memdup_user(uprofile, sizeof(struct su_profile));
    if (IS_ERR(profile)) return PTR_ERR(profile);
    profile->scontext[sizeof(profile->scontext) - 1] = '\0';
    int rc = task_su(pid, profile->to_uid, profile->scontext);
    kvfree(profile);
    return rc;
}

static long supercall(long cmd, long arg1, long arg2, long arg3)
{
    switch (cmd) {
    case SUPERCALL_HELLO:
        logki(SUPERCALL_HELLO_ECHO "\n");
        return SUPERCALL_HELLO_MAGIC;
    case SUPERCALL_KLOG:
        return call_klog((const char *__user)arg1);
    case SUPERCALL_KERNELPATCH_VER:
        return kpver;
    case SUPERCALL_KERNEL_VER:
        return kver;
    }
    logkd("supercall with cmd: %x\n", cmd);
    switch (cmd) {
    case SUPERCALL_SU:
        return call_su((struct su_profile * __user) arg1);
    case SUPERCALL_SU_TASK:
        return call_su_task((pid_t)arg1, (struct su_profile * __user) arg2);
    case SUPERCALL_KPM_LOAD:
        return call_kpm_load((const char *__user)arg1, (const char *__user)arg2);
    case SUPERCALL_KPM_UNLOAD:
        return call_kpm_unload((const char *__user)arg1);
    case SUPERCALL_KPM_NUMS:
        return call_kpm_nums();
    case SUPERCALL_KPM_LIST:
        return call_kpm_list((char *__user)arg1, (int)arg2);
    case SUPERCALL_KPM_INFO:
        return call_kpm_info((const char *__user)arg1, (char *__user)arg2, (int)arg3);
    case SUPERCALL_TEST:
        return call_test(arg1, arg2, arg3);
    }
#ifdef ANDROID
    return supercall_android(cmd, arg1, arg2, arg3);
#endif
    return NO_SYSCALL;
}

static void before(hook_fargs6_t *args, void *udata)
{
    const char *__user ukey = (const char *__user)syscall_argn(args, 0);
    long hash = (long)syscall_argn(args, 1);
    long cmd = (long)syscall_argn(args, 2);
    long a1 = (long)syscall_argn(args, 3);
    long a2 = (long)syscall_argn(args, 4);
    long a3 = (long)syscall_argn(args, 5);

    char key[MAX_KEY_LEN];
    long len = strncpy_from_user_nofault(key, ukey, MAX_KEY_LEN);
    if (len <= 0) return;
    if (superkey_auth(key, len - 1)) return;
    if (hash_key(key) != hash) return;

    args->skip_origin = 1;
    args->ret = supercall(cmd, a1, a2, a3);
}

int supercall_install()
{
    int rc = 0;
    // hook_err_t err = inline_hook_syscalln(__NR_supercall, 6, before, 0, 0);
    hook_err_t err = fp_hook_syscalln(__NR_supercall, 6, before, 0, 0);
    if (err) {
        log_boot("install supercall hook error: %d\n", err);
        rc = err;
        goto out;
    }
out:
    return rc;
}
