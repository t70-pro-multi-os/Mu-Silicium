/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>
#include <stdint.h>
#include <sys/param.h>
#include <sys/mount_internal.h>
#include <sys/proc_internal.h>
#include <sys/filedesc.h>
#include <sys/vnode_internal.h>

/* Measured by this kernel's compiler; the VM observer never guesses private offsets. */
const uint64_t t70_ufs_mount_vtable_offset = offsetof(struct mount, mnt_vtable);
const uint64_t t70_ufs_vfs_name_offset = offsetof(struct vfstable, vfc_name);
const uint64_t t70_ufs_mount_flags_offset = offsetof(struct mount, mnt_flag);
const uint64_t t70_ufs_mnt_rdonly_flag = MNT_RDONLY;
const uint64_t t70_ufs_fs_name_bytes = MFSNAMELEN;

const uint64_t t70_ufs_mount_devvp_offset = offsetof(struct mount, mnt_devvp);
const uint64_t t70_ufs_mnt_rootfs_flag = MNT_ROOTFS;
const uint64_t t70_ufs_proc_pid_offset = offsetof(struct proc, p_pid);
const uint64_t t70_ufs_proc_pid_bytes = sizeof(((struct proc *)0)->p_pid);
const uint64_t t70_ufs_proc_cdir_offset = offsetof(struct proc, p_fd) + offsetof(struct filedesc, fd_cdir);
const uint64_t t70_ufs_proc_rdir_offset = offsetof(struct proc, p_fd) + offsetof(struct filedesc, fd_rdir);
const uint64_t t70_ufs_proc_fd_flags_offset = offsetof(struct proc, p_fd) + offsetof(struct filedesc, fd_flags);
const uint64_t t70_ufs_proc_fd_flags_bytes = sizeof(((struct filedesc *)0)->fd_flags);
const uint64_t t70_ufs_fd_chroot_flag = FD_CHROOT;
const uint64_t t70_ufs_vnode_mount_offset = offsetof(struct vnode, v_mount);
const uint64_t t70_ufs_vnode_type_offset = offsetof(struct vnode, v_type);
const uint64_t t70_ufs_vnode_type_bytes = sizeof(((struct vnode *)0)->v_type);
const uint64_t t70_ufs_vnode_flags_offset = offsetof(struct vnode, v_flag);
const uint64_t t70_ufs_vnode_flags_bytes = sizeof(((struct vnode *)0)->v_flag);
const uint64_t t70_ufs_vdir_type = VDIR;
const uint64_t t70_ufs_vroot_flag = VROOT;
