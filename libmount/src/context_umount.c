/*
 * Copyright (C) 2010 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

/**
 * SECTION: context-umount
 * @title: Umount context
 * @short_description: high-level API to umount operation.
 */

#include <sys/wait.h>
#include <sys/mount.h>

#include "pathnames.h"
#include "loopdev.h"
#include "strutils.h"
#include "mountP.h"

/*
 * umount2 flags
 */
#ifndef MNT_FORCE
# define MNT_FORCE        0x00000001	/* Attempt to forcibly umount */
#endif

#ifndef MNT_DETACH
# define MNT_DETACH       0x00000002	/* Just detach from the tree */
#endif

#ifndef UMOUNT_NOFOLLOW
# define UMOUNT_NOFOLLOW  0x00000008	/* Don't follow symlink on umount */
#endif

#ifndef UMOUNT_UNUSED
# define UMOUNT_UNUSED    0x80000000	/* Flag guaranteed to be unused */
#endif

/*
 * Called by mtab parser to filter out entries, non-zero means that
 * an entry has to be filtered out.
 */
static int mtab_filter(struct libmnt_fs *fs, void *data)
{
	if (!fs || !data)
		return 0;
	if (mnt_fs_streq_target(fs, data))
		return 0;
	if (mnt_fs_streq_srcpath(fs, data))
		return 0;
	return 1;
}

/**
 * mnt_context_find_umount_fs:
 * @cxt: mount context
 * @tgt: mountpoint, device, ...
 * @pfs: returns point to filesystem
 *
 * Returns: 0 on success, <0 on error, 1 if target filesystem not found
 */
int mnt_context_find_umount_fs(struct libmnt_context *cxt,
			       const char *tgt,
			       struct libmnt_fs **pfs)
{
	int rc;
	struct libmnt_table *mtab = NULL;
	struct libmnt_fs *fs;
	struct libmnt_cache *cache = NULL;
	char *cn_tgt = NULL, *loopdev = NULL;

	if (pfs)
		*pfs = NULL;

	if (!cxt || !tgt || !pfs)
		return -EINVAL;

	DBG(CXT, mnt_debug_h(cxt, "umount: lookup FS for '%s'", tgt));

	if (!*tgt)
		return 1; /* empty string is not an error */

	/*
	 * The mtab file may be huge and on systems with utab we have to merge
	 * userspace mount options into /proc/self/mountinfo. This all is
	 * expensive. The mtab filter allows to filter out entries, then
	 * mtab and utab are very tiny files.
	 *
	 * *but*... the filter uses mnt_fs_streq_{target,srcpath} functions
	 * where LABEL, UUID or symlinks are canonicalized. It means that
	 * it's usable only for canonicalized stuff (e.g. kernel mountinfo).
	 */
	if (!cxt->mtab_writable	&& *tgt == '/' &&
	    !mnt_context_is_force(cxt) && !mnt_context_is_lazy(cxt)) {

		struct stat st;

		if (stat(tgt, &st) == 0 && S_ISDIR(st.st_mode)) {
			/* we'll canonicalize /proc/self/mountinfo */
			cache = mnt_context_get_cache(cxt);
			cn_tgt = mnt_resolve_path(tgt, cache);
			if (cn_tgt)
				mnt_context_set_tabfilter(cxt, mtab_filter, cn_tgt);
		}
	}
	rc = mnt_context_get_mtab(cxt, &mtab);

	if (cn_tgt) {
		mnt_context_set_tabfilter(cxt, NULL, NULL);
		if (!cache)
			free(cn_tgt);
	}

	if (rc) {
		DBG(CXT, mnt_debug_h(cxt, "umount: failed to read mtab"));
		return rc;
	}

	if (mnt_table_get_nents(mtab) == 0) {
		DBG(CXT, mnt_debug_h(cxt, "umount: mtab empty"));
		return 1;
	}

try_loopdev:
	fs = mnt_table_find_target(mtab, tgt, MNT_ITER_BACKWARD);
	if (!fs && mnt_context_is_swapmatch(cxt)) {
		/*
		 * Maybe the option is source rather than target (sometimes
		 * people use e.g. "umount /dev/sda1")
		 */
		fs = mnt_table_find_source(mtab, tgt, MNT_ITER_BACKWARD);

		if (fs) {
			struct libmnt_fs *fs1 = mnt_table_find_target(mtab,
							mnt_fs_get_target(fs),
							MNT_ITER_BACKWARD);
			if (!fs1) {
				DBG(CXT, mnt_debug_h(cxt, "mtab is broken?!?!"));
				rc = -EINVAL;
				goto err;
			}
			if (fs != fs1) {
				/* Something was stacked over `file' on the
				 * same mount point. */
				DBG(CXT, mnt_debug_h(cxt,
						"umount: %s: %s is mounted "
						"over it on the same point",
						tgt, mnt_fs_get_source(fs1)));
				rc = -EINVAL;
				goto err;
			}
		}
	}

	if (!fs && !loopdev && mnt_context_is_swapmatch(cxt)) {
		/*
		 * Maybe the option is /path/file.img, try to convert to /dev/loopN
		 */
		struct stat st;

		if (stat(tgt, &st) == 0 && S_ISREG(st.st_mode)) {
			int count;
			const char *bf = cache ? mnt_resolve_path(tgt, cache) : tgt;

			count = loopdev_count_by_backing_file(bf, &loopdev);
			if (count == 1) {
				DBG(CXT, mnt_debug_h(cxt,
					"umount: %s --> %s (retry)", tgt, loopdev));
				tgt = loopdev;
				goto try_loopdev;

			} else if (count > 1)
				DBG(CXT, mnt_debug_h(cxt,
					"umount: warning: %s is associated "
					"with more than one loopdev", tgt));
		}
	}

	if (pfs)
		*pfs = fs;
	free(loopdev);

	DBG(CXT, mnt_debug_h(cxt, "umount fs: %s", fs ? mnt_fs_get_target(fs) :
							"<not found>"));
	return fs ? 0 : 1;
err:
	free(loopdev);
	return rc;
}

/* this is umount replacement to mnt_context_apply_fstab(), use
 * mnt_context_tab_applied() to check result.
 */
static int lookup_umount_fs(struct libmnt_context *cxt)
{
	const char *tgt;
	struct libmnt_fs *fs = NULL;
	int rc;

	assert(cxt);
	assert(cxt->fs);

	tgt = mnt_fs_get_target(cxt->fs);
	if (!tgt) {
		DBG(CXT, mnt_debug_h(cxt, "umount: undefined target"));
		return -EINVAL;
	}

	rc = mnt_context_find_umount_fs(cxt, tgt, &fs);
	if (rc < 0)
		return rc;
	if (rc == 1 || !fs) {
		DBG(CXT, mnt_debug_h(cxt, "umount: cannot find '%s' in mtab", tgt));
		return 0;	/* this is correct! */
	}

	if (fs != cxt->fs) {
		/* copy from mtab to our FS description
		 */
		mnt_fs_set_source(cxt->fs, NULL);
		mnt_fs_set_target(cxt->fs, NULL);

		if (!mnt_copy_fs(cxt->fs, fs)) {
			DBG(CXT, mnt_debug_h(cxt, "umount: failed to copy FS"));
			return -errno;
		}
		DBG(CXT, mnt_debug_h(cxt, "umount: mtab applied"));
	}

	cxt->flags |= MNT_FL_TAB_APPLIED;
	return rc;
}

/* check if @devname is loopdev and if the device is associated
 * with a source from @fstab_fs
 */
static int is_associated_fs(const char *devname, struct libmnt_fs *fs)
{
	uintmax_t offset = 0;
	const char *src;
	char *val, *optstr;
	size_t valsz;
	int flags = 0;

	/* check if it begins with /dev/loop */
	if (strncmp(devname, _PATH_DEV_LOOP, sizeof(_PATH_DEV_LOOP) - 1))
		return 0;

	src = mnt_fs_get_srcpath(fs);
	if (!src)
		return 0;

	/* check for the offset option in @fs */
	optstr = (char *) mnt_fs_get_user_options(fs);

	if (optstr &&
	    mnt_optstr_get_option(optstr, "offset", &val, &valsz) == 0) {
		flags |= LOOPDEV_FL_OFFSET;

		if (mnt_parse_offset(val, valsz, &offset) != 0)
			return 0;
	}

	return loopdev_is_used(devname, src, offset, flags);
}

static int prepare_helper_from_options(struct libmnt_context *cxt,
				       const char *name)
{
	char *suffix = NULL;
	const char *opts;
	size_t valsz;

	if (mnt_context_is_nohelpers(cxt))
		return 0;

	opts = mnt_fs_get_user_options(cxt->fs);
	if (!opts)
		return 0;

	if (mnt_optstr_get_option(opts, name, &suffix, &valsz))
		return 0;

	suffix = strndup(suffix, valsz);
	if (!suffix)
		return -ENOMEM;

	DBG(CXT, mnt_debug_h(cxt, "umount: umount.%s %s requested", suffix, name));

	return mnt_context_prepare_helper(cxt, "umount", suffix);
}

/*
 * Note that cxt->fs contains relevant mtab entry!
 */
static int evaluate_permissions(struct libmnt_context *cxt)
{
	struct libmnt_table *fstab;
	unsigned long u_flags = 0;
	const char *tgt, *src, *optstr;
	int rc, ok = 0;
	struct libmnt_fs *fs;

	assert(cxt);
	assert(cxt->fs);
	assert((cxt->flags & MNT_FL_MOUNTFLAGS_MERGED));

	if (!cxt || !cxt->fs)
		return -EINVAL;

	if (!mnt_context_is_restricted(cxt))
		 return 0;		/* superuser mount */

	DBG(CXT, mnt_debug_h(cxt, "umount: evaluating permissions"));

	if (!mnt_context_tab_applied(cxt)) {
		DBG(CXT, mnt_debug_h(cxt,
				"cannot find %s in mtab and you are not root",
				mnt_fs_get_target(cxt->fs)));
		goto eperm;
	}

	if (cxt->user_mountflags & MNT_MS_UHELPER) {
		/* on uhelper= mount option based helper */
		rc = prepare_helper_from_options(cxt, "uhelper");
		if (rc)
			return rc;
		if (cxt->helper)
			return 0;	/* we'll call /sbin/umount.<uhelper> */
	}

	/*
	 * User mounts have to be in /etc/fstab
	 */
	rc = mnt_context_get_fstab(cxt, &fstab);
	if (rc)
		return rc;

	tgt = mnt_fs_get_target(cxt->fs);
	src = mnt_fs_get_source(cxt->fs);

	if (mnt_fs_get_bindsrc(cxt->fs)) {
		src = mnt_fs_get_bindsrc(cxt->fs);
		DBG(CXT, mnt_debug_h(cxt,
				"umount: using bind source: %s", src));
	}

	/* If fstab contains the two lines
	 *	/dev/sda1 /mnt/zip auto user,noauto  0 0
	 *	/dev/sda4 /mnt/zip auto user,noauto  0 0
	 * then "mount /dev/sda4" followed by "umount /mnt/zip" used to fail.
	 * So, we must not look for the file, but for the pair (dev,file) in fstab.
	  */
	fs = mnt_table_find_pair(fstab, src, tgt, MNT_ITER_FORWARD);
	if (!fs) {
		/*
		 * It's possible that there is /path/file.img in fstab and
		 * /dev/loop0 in mtab -- then we have to check the relation
		 * between loopdev and the file.
		 */
		fs = mnt_table_find_target(fstab, tgt, MNT_ITER_FORWARD);
		if (fs) {
			struct libmnt_cache *cache = mnt_context_get_cache(cxt);
			const char *sp = mnt_fs_get_srcpath(cxt->fs);		/* devname from mtab */
			const char *dev = sp && cache ? mnt_resolve_path(sp, cache) : sp;

			if (!dev || !is_associated_fs(dev, fs))
				fs = NULL;
		}
		if (!fs) {
			DBG(CXT, mnt_debug_h(cxt,
					"umount %s: mtab disagrees with fstab",
					tgt));
			goto eperm;
		}
	}

	/*
	 * User mounting and unmounting is allowed only if fstab contains one
	 * of the options `user', `users' or `owner' or `group'.
	 *
	 * The option `users' allows arbitrary users to mount and unmount -
	 * this may be a security risk.
	 *
	 * The options `user', `owner' and `group' only allow unmounting by the
	 * user that mounted (visible in mtab).
	 */
	optstr = mnt_fs_get_user_options(fs);	/* FSTAB mount options! */
	if (!optstr)
		goto eperm;

	if (mnt_optstr_get_flags(optstr, &u_flags,
				mnt_get_builtin_optmap(MNT_USERSPACE_MAP)))
		goto eperm;

	if (u_flags & MNT_MS_USERS) {
		DBG(CXT, mnt_debug_h(cxt,
			"umount: promiscuous setting ('users') in fstab"));
		return 0;
	}
	/*
	 * Check user=<username> setting from mtab if there is a user, owner or
	 * group option in /etc/fstab
	 */
	if (u_flags & (MNT_MS_USER | MNT_MS_OWNER | MNT_MS_GROUP)) {

		char *curr_user = NULL;
		char *mtab_user = NULL;
		size_t sz;

		DBG(CXT, mnt_debug_h(cxt,
				"umount: checking user=<username> from mtab"));

		curr_user = mnt_get_username(getuid());

		if (!curr_user) {
			DBG(CXT, mnt_debug_h(cxt, "umount %s: cannot "
				"convert %d to username", tgt, getuid()));
			goto eperm;
		}

		/* get options from mtab */
		optstr = mnt_fs_get_user_options(cxt->fs);
		if (optstr && !mnt_optstr_get_option(optstr,
					"user", &mtab_user, &sz) && sz)
			ok = !strncmp(curr_user, mtab_user, sz);

		free(curr_user);
	}

	if (ok) {
		DBG(CXT, mnt_debug_h(cxt, "umount %s is allowed", tgt));
		return 0;
	}
eperm:
	DBG(CXT, mnt_debug_h(cxt, "umount is not allowed for you"));
	return -EPERM;
}

static int exec_helper(struct libmnt_context *cxt)
{
	int rc;

	assert(cxt);
	assert(cxt->fs);
	assert(cxt->helper);
	assert((cxt->flags & MNT_FL_MOUNTFLAGS_MERGED));
	assert(cxt->helper_exec_status == 1);

	DBG_FLUSH;

	switch (fork()) {
	case 0:
	{
		const char *args[10], *type;
		int i = 0;

		if (setgid(getgid()) < 0)
			exit(EXIT_FAILURE);

		if (setuid(getuid()) < 0)
			exit(EXIT_FAILURE);

		type = mnt_fs_get_fstype(cxt->fs);

		args[i++] = cxt->helper;			/* 1 */
		args[i++] = mnt_fs_get_target(cxt->fs);		/* 2 */

		if (mnt_context_is_nomtab(cxt))
			args[i++] = "-n";			/* 3 */
		if (mnt_context_is_lazy(cxt))
			args[i++] = "-l";			/* 4 */
		if (mnt_context_is_force(cxt))
			args[i++] = "-f";			/* 5 */
		if (mnt_context_is_verbose(cxt))
			args[i++] = "-v";			/* 6 */
		if (mnt_context_is_rdonly_umount(cxt))
			args[i++] = "-r";			/* 7 */
		if (type && !endswith(cxt->helper, type)) {
			args[i++] = "-t";			/* 8 */
			args[i++] = (char *) type;	/* 9 */
		}

		args[i] = NULL;					/* 10 */
#ifdef CONFIG_LIBMOUNT_DEBUG
		for (i = 0; args[i]; i++)
			DBG(CXT, mnt_debug_h(cxt, "argv[%d] = \"%s\"",
							i, args[i]));
#endif
		DBG_FLUSH;
		execv(cxt->helper, (char * const *) args);
		exit(EXIT_FAILURE);
	}
	default:
	{
		int st;
		wait(&st);
		cxt->helper_status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

		DBG(CXT, mnt_debug_h(cxt, "%s executed [status=%d]",
					cxt->helper, cxt->helper_status));
		cxt->helper_exec_status = rc = 0;
		break;
	}

	case -1:
		cxt->helper_exec_status = rc = -errno;
		DBG(CXT, mnt_debug_h(cxt, "fork() failed"));
		break;
	}

	return rc;
}

/*
 * mnt_context_helper_setopt() backend.
 *
 * This function applies umount.type command line option (for example parsed
 * by getopt() or getopt_long()) to @cxt. All unknown options are ignored and
 * then 1 is returned.
 *
 * Returns: negative number on error, 1 if @c is unknown option, 0 on success.
 */
int mnt_context_umount_setopt(struct libmnt_context *cxt, int c, char *arg)
{
	int rc = -EINVAL;

	assert(cxt);
	assert(cxt->action == MNT_ACT_UMOUNT);

	switch(c) {
	case 'n':
		rc = mnt_context_disable_mtab(cxt, TRUE);
		break;
	case 'l':
		rc = mnt_context_enable_lazy(cxt, TRUE);
		break;
	case 'f':
		rc = mnt_context_enable_force(cxt, TRUE);
		break;
	case 'v':
		rc = mnt_context_enable_verbose(cxt, TRUE);
		break;
	case 'r':
		rc = mnt_context_enable_rdonly_umount(cxt, TRUE);
		break;
	case 't':
		if (arg)
			rc = mnt_context_set_fstype(cxt, arg);
		break;
	default:
		return 1;
		break;
	}

	return rc;
}

/* Check whether the kernel supports the UMOUNT_NOFOLLOW flag */
static int umount_nofollow_support(void)
{
	int res = umount2("", UMOUNT_UNUSED);
	if (res != -1 || errno != EINVAL)
		return 0;

	res = umount2("", UMOUNT_NOFOLLOW);
	if (res != -1 || errno != ENOENT)
		return 0;

	return 1;
}

static int do_umount(struct libmnt_context *cxt)
{
	int rc = 0, flags = 0;
	const char *src, *target;
	char *tgtbuf = NULL;

	assert(cxt);
	assert(cxt->fs);
	assert((cxt->flags & MNT_FL_MOUNTFLAGS_MERGED));
	assert(cxt->syscall_status == 1);

	if (cxt->helper)
		return exec_helper(cxt);

	src = mnt_fs_get_srcpath(cxt->fs);
	target = mnt_fs_get_target(cxt->fs);

	if (!target)
		return -EINVAL;

	DBG(CXT, mnt_debug_h(cxt, "do umount"));

	if (cxt->restricted && !mnt_context_is_fake(cxt)) {
		/*
		 * extra paranoia for non-root users
		 * -- chdir to the parent of the mountpoint and use NOFOLLOW
		 *    flag to avoid races and symlink attacks.
		 */
		if (umount_nofollow_support())
			flags |= UMOUNT_NOFOLLOW;

		rc = mnt_chdir_to_parent(target, &tgtbuf);
		if (rc)
			return rc;
		target = tgtbuf;
	}

	if (mnt_context_is_lazy(cxt))
		flags |= MNT_DETACH;

	else if (mnt_context_is_force(cxt))
		flags |= MNT_FORCE;

	DBG(CXT, mnt_debug_h(cxt, "umount(2) [target='%s', flags=0x%08x]%s",
				target, flags,
				mnt_context_is_fake(cxt) ? " (FAKE)" : ""));

	if (mnt_context_is_fake(cxt))
		rc = 0;
	else {
		rc = flags ? umount2(target, flags) : umount(target);
		if (rc < 0)
			cxt->syscall_status = -errno;
		free(tgtbuf);
	}

	/*
	 * try remount read-only
	 */
	if (rc < 0
	    && cxt->syscall_status == -EBUSY
	    && mnt_context_is_rdonly_umount(cxt)
	    && src) {

		mnt_context_set_mflags(cxt, (cxt->mountflags |
					     MS_REMOUNT | MS_RDONLY));
		mnt_context_enable_loopdel(cxt, FALSE);

		DBG(CXT, mnt_debug_h(cxt,
			"umount(2) failed [errno=%d] -- trying to remount read-only",
			-cxt->syscall_status));

		rc = mount(src, mnt_fs_get_target(cxt->fs), NULL,
			    MS_MGC_VAL | MS_REMOUNT | MS_RDONLY, NULL);
		if (rc < 0) {
			cxt->syscall_status = -errno;
			DBG(CXT, mnt_debug_h(cxt,
				"read-only re-mount(2) failed [errno=%d]",
				-cxt->syscall_status));

			return -cxt->syscall_status;
		}
		cxt->syscall_status = 0;
		DBG(CXT, mnt_debug_h(cxt, "read-only re-mount(2) success"));
		return 0;
	}

	if (rc < 0) {
		DBG(CXT, mnt_debug_h(cxt, "umount(2) failed [errno=%d]",
			-cxt->syscall_status));
		return -cxt->syscall_status;
	}

	cxt->syscall_status = 0;
	DBG(CXT, mnt_debug_h(cxt, "umount(2) success"));
	return 0;
}

/**
 * mnt_context_prepare_umount:
 * @cxt: mount context
 *
 * Prepare context for umounting, unnecessary for mnt_context_umount().
 *
 * Returns: 0 on success, and negative number in case of error.
 */
int mnt_context_prepare_umount(struct libmnt_context *cxt)
{
	int rc;

	assert(cxt);
	assert(cxt->fs);
	assert(cxt->helper_exec_status == 1);
	assert(cxt->syscall_status == 1);

	if (!cxt || !cxt->fs || mnt_fs_is_swaparea(cxt->fs))
		return -EINVAL;
	if (!mnt_context_get_source(cxt) && !mnt_context_get_target(cxt))
		return -EINVAL;
	if (cxt->flags & MNT_FL_PREPARED)
		return 0;

	free(cxt->helper);	/* be paranoid */
	cxt->helper = NULL;
	cxt->action = MNT_ACT_UMOUNT;

	rc = lookup_umount_fs(cxt);
	if (!rc)
		rc = mnt_context_merge_mflags(cxt);
	if (!rc)
		rc = evaluate_permissions(cxt);

	if (!rc && !cxt->helper) {

		if (cxt->user_mountflags & MNT_MS_HELPER)
			/* on helper= mount option based helper */
			rc = prepare_helper_from_options(cxt, "helper");

		if (!rc && !cxt->helper)
			/* on fstype based helper */
			rc = mnt_context_prepare_helper(cxt, "umount", NULL);
	}

	if (!rc && (cxt->user_mountflags & MNT_MS_LOOP))
		/* loop option explicitly specified in mtab, detach this loop */
		mnt_context_enable_loopdel(cxt, TRUE);

	if (!rc && mnt_context_is_loopdel(cxt) && cxt->fs) {
		const char *src = mnt_fs_get_srcpath(cxt->fs);

		if (src && (!is_loopdev(src) || loopdev_is_autoclear(src)))
			mnt_context_enable_loopdel(cxt, FALSE);
	}

	if (rc) {
		DBG(CXT, mnt_debug_h(cxt, "umount: preparing failed"));
		return rc;
	}
	cxt->flags |= MNT_FL_PREPARED;
	return rc;
}

/**
 * mnt_context_do_umount:
 * @cxt: mount context
 *
 * Umount filesystem by umount(2) or fork()+exec(/sbin/umount.type).
 * Unnecessary for mnt_context_umount().
 *
 * See also mnt_context_disable_helpers().
 *
 * WARNING: non-zero return code does not mean that umount(2) syscall or
 *          umount.type helper wasn't successfully called.
 *
 *          Check mnt_context_get_status() after error!
*
 * Returns: 0 on success;
 *         >0 in case of umount(2) error (returns syscall errno),
 *         <0 in case of other errors.
 */
int mnt_context_do_umount(struct libmnt_context *cxt)
{
	int rc;

	assert(cxt);
	assert(cxt->fs);
	assert(cxt->helper_exec_status == 1);
	assert(cxt->syscall_status == 1);
	assert((cxt->flags & MNT_FL_PREPARED));
	assert((cxt->action == MNT_ACT_UMOUNT));
	assert((cxt->flags & MNT_FL_MOUNTFLAGS_MERGED));

	rc = do_umount(cxt);
	if (rc)
		return rc;

	if (mnt_context_get_status(cxt) && !mnt_context_is_fake(cxt)) {
		/*
		 * Umounted, do some post-umount operations
		 *	- remove loopdev
		 *	- refresh in-memory mtab stuff if remount rather than
		 *	  umount has been performed
		 */
		if (mnt_context_is_loopdel(cxt)
		    && !(cxt->mountflags & MS_REMOUNT))
			rc = mnt_context_delete_loopdev(cxt);

		if (!mnt_context_is_nomtab(cxt)
		    && mnt_context_get_status(cxt)
		    && !cxt->helper
		    && mnt_context_is_rdonly_umount(cxt)
		    && (cxt->mountflags & MS_REMOUNT)) {

			/* use "remount" instead of "umount" in /etc/mtab */
			if (!rc && cxt->update && cxt->mtab_writable)
				rc = mnt_update_set_fs(cxt->update,
						       cxt->mountflags, NULL, cxt->fs);
		}
	}
	return rc;
}

/**
 * mnt_context_finalize_umount:
 * @cxt: context
 *
 * Mtab update, etc. Unnecessary for mnt_context_umount(), but should be called
 * after mnt_context_do_umount(). See also mnt_context_set_syscall_status().
 *
 * Returns: negative number on error, 0 on success.
 */
int mnt_context_finalize_umount(struct libmnt_context *cxt)
{
	int rc;

	assert(cxt);
	assert(cxt->fs);
	assert((cxt->flags & MNT_FL_PREPARED));
	assert((cxt->flags & MNT_FL_MOUNTFLAGS_MERGED));

	rc = mnt_context_prepare_update(cxt);
	if (!rc)
		rc = mnt_context_update_tabs(cxt);;
	return rc;
}


/**
 * mnt_context_umount:
 * @cxt: umount context
 *
 * High-level, umounts filesystem by umount(2) or fork()+exec(/sbin/umount.type).
 *
 * This is similar to:
 *
 *	mnt_context_prepare_umount(cxt);
 *	mnt_context_do_umount(cxt);
 *	mnt_context_finalize_umount(cxt);
 *
 * See also mnt_context_disable_helpers().
 *
 * WARNING: non-zero return code does not mean that umount(2) syscall or
 *          umount.type helper wasn't successfully called.
 *
 *          Check mnt_context_get_status() after error!
 *
 * Returns: 0 on success;
 *         >0 in case of umount(2) error (returns syscall errno),
 *         <0 in case of other errors.
 */
int mnt_context_umount(struct libmnt_context *cxt)
{
	int rc;

	assert(cxt);
	assert(cxt->fs);
	assert(cxt->helper_exec_status == 1);
	assert(cxt->syscall_status == 1);

	DBG(CXT, mnt_debug_h(cxt, "umount: %s", mnt_context_get_target(cxt)));

	rc = mnt_context_prepare_umount(cxt);
	if (!rc)
		rc = mnt_context_prepare_update(cxt);
	if (!rc)
		rc = mnt_context_do_umount(cxt);
	if (!rc)
		rc = mnt_context_update_tabs(cxt);
	return rc;
}


/**
 * mnt_context_next_umount:
 * @cxt: context
 * @itr: iterator
 * @fs: returns the current filesystem
 * @mntrc: returns the return code from mnt_context_umount()
 * @ignored: returns 1 for not matching
 *
 * This function tries to umount the next filesystem from mtab (as returned by
 * mnt_context_get_mtab()).
 *
 * You can filter out filesystems by:
 *	mnt_context_set_options_pattern() to simulate umount -a -O pattern
 *	mnt_context_set_fstype_pattern()  to simulate umount -a -t pattern
 *
 * If the filesystem is not mounted or does not match the defined criteria,
 * then the function mnt_context_next_umount() returns zero, but the @ignored is
 * non-zero. Note that the root filesystem is always ignored.
 *
 * If umount(2) syscall or umount.type helper failed, then the
 * mnt_context_next_umount() function returns zero, but the @mntrc is non-zero.
 * Use also mnt_context_get_status() to check if the filesystem was
 * successfully umounted.
 *
 * Returns: 0 on success,
 *         <0 in case of error (!= umount(2) errors)
 *          1 at the end of the list.
 */
int mnt_context_next_umount(struct libmnt_context *cxt,
			   struct libmnt_iter *itr,
			   struct libmnt_fs **fs,
			   int *mntrc,
			   int *ignored)
{
	struct libmnt_table *mtab;
	const char *tgt;
	int rc;

	if (ignored)
		*ignored = 0;
	if (mntrc)
		*mntrc = 0;

	if (!cxt || !fs || !itr)
		return -EINVAL;

	rc = mnt_context_get_mtab(cxt, &mtab);
	cxt->mtab = NULL;		/* do not reset mtab */
	mnt_reset_context(cxt);
	cxt->mtab = mtab;

	if (rc)
		return rc;

	do {
		rc = mnt_table_next_fs(mtab, itr, fs);
		if (rc != 0)
			return rc;	/* no more filesystems (or error) */

		tgt = mnt_fs_get_target(*fs);
	} while (!tgt);

	DBG(CXT, mnt_debug_h(cxt, "next-umount: trying %s", tgt));

	/* ignore filesystems which don't match options patterns */
	if ((cxt->fstype_pattern && !mnt_fs_match_fstype(*fs,
					cxt->fstype_pattern)) ||

	/* ignore filesystems which don't match type patterns */
	   (cxt->optstr_pattern && !mnt_fs_match_options(*fs,
					cxt->optstr_pattern))) {
		if (ignored)
			*ignored = 1;
		DBG(CXT, mnt_debug_h(cxt, "next-umount: not-match "
				"[fstype: %s, t-pattern: %s, options: %s, O-pattern: %s]",
				mnt_fs_get_fstype(*fs),
				cxt->fstype_pattern,
				mnt_fs_get_options(*fs),
				cxt->optstr_pattern));
		return 0;
	}

	rc = mnt_context_set_fs(cxt, *fs);
	if (rc)
		return rc;
	rc = mnt_context_umount(cxt);
	if (mntrc)
		*mntrc = rc;
	return 0;
}
