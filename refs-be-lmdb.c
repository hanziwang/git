/*
 * This file implements a lmdb backend for refs.
 *
 * The design of this backend relies on lmdb's write lock -- that is, any
 * write transaction blocks all other writers.  Thus, as soon as a ref
 * transaction is opened, we know that any values we read won't
 * change out from under us, and we have a fully-consistent view of the
 * database.
 *
 * We store the content of refs including the trailing \0 so that
 * standard C string functions can handle them.  Just like struct
 * strbuf.
 */
#include <lmdb.h>
#include <sys/uio.h>
#include "cache.h"
#include "object.h"
#include "refs.h"
#include "tag.h"
#include "lockfile.h"
#include "run-command.h"

static struct trace_key db_trace = TRACE_KEY_INIT(LMDB);

static MDB_env *env;

static char *db_path;

struct lmdb_transaction_info {
	MDB_txn *txn;
	MDB_dbi dbi;
	const char *submodule;
};

struct lmdb_transaction {
	struct ref_transaction base;
	MDB_cursor *cursor;
	struct lmdb_transaction_info info;
	struct hashmap updated_refs;
	unsigned int flags;
};

static struct lmdb_transaction transaction = {{}, NULL};

struct ref_update {
	struct hashmap_entry ent;  /* must be first */
	size_t len;
	char refname[FLEX_ARRAY];
};

static char *get_refdb_path(const char *base)
{
	struct strbuf path_buf = STRBUF_INIT;
	strbuf_addf(&path_buf, "%s/refdb", base);
	return strbuf_detach(&path_buf, NULL);
}

static int in_write_transaction(void)
{
	return transaction.info.txn && !(transaction.flags & MDB_RDONLY);
}

static void init_env(MDB_env **env, const char *path)
{
	int ret;
	if (*env)
		return;

	if ((ret = mdb_env_create(env)) != MDB_SUCCESS)
		die("mdb_env_create failed: %s", mdb_strerror(ret));
	if ((ret = mdb_env_set_maxreaders(*env, 1000)) != MDB_SUCCESS)
		die("BUG: mdb_env_set_maxreaders failed: %s", mdb_strerror(ret));
	if ((ret = mdb_env_set_mapsize(*env, (1<<30))) != MDB_SUCCESS)
		die("BUG: mdb_set_mapsize failed: %s", mdb_strerror(ret));
	if ((ret = mdb_env_open(*env, path, 0 , 0664)) != MDB_SUCCESS)
		die("BUG: mdb_env_open (%s) failed: %s", path, mdb_strerror(ret));
}

static int lmdb_initdb(struct strbuf *err, int shared)
{
	/* To create a db, all we need to do is make a directory for
	   it to live in; lmdb will do the rest. */

	assert(db_path);
	if (mkdir(db_path, 0775)) {
		if (errno != EEXIST) {
			strbuf_addf(err, "%s", strerror(errno));
			return -1;
		}
	}
	return 0;
}

static void lmdb_init_backend(struct refdb_config_data *data)
{
	if (db_path)
		return;

	db_path = xstrdup(real_path(get_refdb_path(data->refs_base)));
	trace_printf_key(&db_trace, "Init backend\n");
}

int ref_update_cmp(const void *entry, const void *entry_or_key, const void *keydata)
{

	const struct ref_update *existing = entry;
	const struct ref_update *incoming = entry_or_key;

	return existing->len != incoming->len ||
		memcmp(existing->refname, incoming->refname, existing->len);
}

static void mdb_cursor_open_or_die(struct lmdb_transaction_info *info,
				   MDB_cursor **cursor)
{
	int ret = mdb_cursor_open(info->txn, info->dbi, cursor);
	if (ret)
		die("mdb_cursor_open failed: %s", mdb_strerror(ret));
}

static void submodule_path(struct strbuf *sb, const char *submodule,
			   const char *refname)
{
	if (submodule)
		strbuf_git_path_submodule(sb, submodule, "%s", refname);
	else
		strbuf_git_path(sb, "%s", refname);
}

static int read_per_worktree_ref(const char *submodule, const char *refname,
				 struct MDB_val *val)
{
	static struct strbuf sb = STRBUF_INIT;
	static struct strbuf path = STRBUF_INIT;

	submodule_path(&path, submodule, refname);

	if (strbuf_read_file(&sb, path.buf, 200) < 0) {
		strbuf_release(&sb);
		strbuf_release(&path);
		if (errno == ENOENT)
			return MDB_NOTFOUND;
		return -1;
	}
	strbuf_setlen(&sb, sb.len - 1);

	val->mv_data = sb.buf;
	val->mv_size = sb.len + 1;

	strbuf_detach(&sb, NULL);
	strbuf_release(&path);
	return 0;
}

static void write_per_worktree_ref(const char *submodule, const char *refname,
				   MDB_val *val)
{
	static struct lock_file lock;
	int fd;
	int len = val->mv_size - 1;
	struct strbuf path = STRBUF_INIT;

	submodule_path(&path, submodule, refname);
	safe_create_leading_directories(path.buf);

	fd = hold_lock_file_for_update(&lock, path.buf, LOCK_DIE_ON_ERROR);
	strbuf_release(&path);

	if (write_in_full(fd, val->mv_data, len) != len ||
	    write_in_full(fd, "\n", 1) != 1)
		die_errno("failed to write new HEAD");

	if (commit_lock_file(&lock))
		die_errno("failed to write new HEAD");
}

static int del_per_worktree_ref(const char *submodule, const char *refname,
				MDB_val *val)
{
	struct strbuf path = STRBUF_INIT;
	int result;

	/*
	 * Returning deleted ref data is not yet implemented, but no
	 * callers need it.
	 */
	assert(val == NULL);

	submodule_path(&path, submodule, refname);

	result = unlink(path.buf);
	strbuf_release(&path);
	if (result) {
		if (errno == ENOENT)
			return 0;
		return 1;
	}

	return 0;
}

static int mdb_get_or_die(struct lmdb_transaction_info *info, MDB_val *key, MDB_val *val)
{
	int ret;

	if (ref_type(key->mv_data) != REF_TYPE_NORMAL)
		return read_per_worktree_ref(info->submodule, key->mv_data, val);

	ret = mdb_get(info->txn, info->dbi, key, val);
	if (ret) {
		if (ret != MDB_NOTFOUND)
			die("mdb_get failed: %s", mdb_strerror(ret));
		return ret;
	}
	return 0;
}

static int mdb_del_or_die(struct lmdb_transaction_info *info, MDB_val *key, MDB_val *val)
{
	int ret;

	if (ref_type(key->mv_data) != REF_TYPE_NORMAL)
		return del_per_worktree_ref(info->submodule, key->mv_data, val);

	ret = mdb_del(info->txn, info->dbi, key, val);
	if (ret) {
		if (ret != MDB_NOTFOUND)
			die("mdb_del failed: %s", mdb_strerror(ret));
		return ret;
	}
	return 0;
}

static void mdb_put_or_die(struct lmdb_transaction_info *info, MDB_val *key, MDB_val *val, int mode)
{
	int ret;

	if (ref_type(key->mv_data) != REF_TYPE_NORMAL) {
		write_per_worktree_ref(info->submodule, key->mv_data, val);
		return;
	}
	assert(val->mv_size == 0 || ((char *)val->mv_data)[val->mv_size - 1] == 0);

	ret = mdb_put(info->txn, info->dbi, key, val, mode);
	if (ret) {
		if (ret == MDB_BAD_VALSIZE)
			die("Ref name %s too long (max size is %d)",
			    (const char *)key->mv_data,
			    mdb_env_get_maxkeysize(env));
		else
			die("mdb_put failed: %s", mdb_strerror(ret));
	}
}

static int mdb_cursor_get_or_die(MDB_cursor *cursor, MDB_val *key, MDB_val *val, int mode)
{
	int ret;

	ret = mdb_cursor_get(cursor, key, val, mode);
	if (ret) {
		if (ret != MDB_NOTFOUND)
			die("mdb_cursor_get failed: %s", mdb_strerror(ret));
		return ret;
	}
	assert(((char *)val->mv_data)[val->mv_size - 1] == 0);
	return 0;
}

static int mdb_cursor_del_or_die(MDB_cursor *cursor, int flags)
{
	int ret = mdb_cursor_del(cursor, flags);
	if (ret) {
		if (ret != MDB_NOTFOUND)
			die("mdb_cursor_del failed: %s", mdb_strerror(ret));
		return ret;
	}
	return 0;
}

/*
 * Begin a transaction. Because only one transaction per thread is
 * permitted, we use a global transaction object.  If a read-write
 * transaction is presently already in-progress, and a read-only
 * transaction is requested, the read-write transaction will be
 * returned instead.  If a read-write transaction is requested and a
 * read-only transaction is open, the read-only transaction will be
 * closed.
 *
 * It is a bug to request a read-write transaction during another
 * read-write transaction.
 *
 * As a result, it is unsafe to retain read-only transactions past the
 * point where a read-write transaction might be needed.  For
 * instance, any call that has callbacks outside this module must
 * conclude all of its reads from the database before calling those
 * callbacks, or must reacquire the transaction after its callbacks
 * are completed.
 */
int lmdb_transaction_begin_flags(struct strbuf *err, unsigned int flags)
{
	int ret;
	MDB_txn *txn;
	static int last_commands_run = 0;
	int force_restart = 0;

	init_env(&env, db_path);

	if (total_commands_run != last_commands_run) {
		/*
		 * Since each transaction sees a consistent view of
		 * the db, downstream processes that write the db
		 * won't be seen in this transaction.  We don't know
		 * whether any given downstream process has made any
		 * writes, so if there have been any downstream processes,
		 * we had better reopen the transaction.
		 */
		force_restart = 1;
		last_commands_run = total_commands_run;
	}

	if (!transaction.info.txn) {
		hashmap_init(&transaction.updated_refs, ref_update_cmp, 0);
		if ((ret = mdb_txn_begin(env, NULL, flags, &txn)) != MDB_SUCCESS) {
			strbuf_addf(err, "mdb_txn_begin failed: %s",
				    mdb_strerror(ret));
			return -1;
		}
		if ((ret = mdb_dbi_open(txn, NULL, 0, &transaction.info.dbi)) != MDB_SUCCESS) {
			strbuf_addf(err, "mdb_txn_open failed: %s",
				    mdb_strerror(ret));
			return -1;
		}
		transaction.info.txn = txn;
		transaction.flags = flags;
		return 0;
	}

	if (transaction.flags == flags && !(flags & MDB_RDONLY)) {
		die("BUG: rw transaction started during another rw txn");
	}

	if (force_restart || (transaction.flags != flags && transaction.flags & MDB_RDONLY)) {
		/*
		 * RO -> RW, or forced restart due to possible changes
		 * from downstream processes.
		 */
		mdb_txn_abort(transaction.info.txn);
		if ((ret = mdb_txn_begin(env, NULL, flags, &txn)) != MDB_SUCCESS) {
			strbuf_addf(err, "restarting txn: mdb_txn_begin failed: %s",
				    mdb_strerror(ret));
			return -1;
		}
		if ((ret = mdb_dbi_open(txn, NULL, 0, &transaction.info.dbi)) != MDB_SUCCESS) {
			strbuf_addf(err, "mdb_txn_open failed: %s",
				    mdb_strerror(ret));
			return -1;
		}
		transaction.info.txn = txn;
		transaction.flags = flags;
	}
	/* RW -> RO just keeps the RW txn */
	return 0;
}

static struct ref_transaction *lmdb_transaction_begin_flags_or_die(int flags)
{
	struct strbuf err = STRBUF_INIT;
	if (lmdb_transaction_begin_flags(&err, flags))
		die("%s", err.buf);
	return (struct ref_transaction *)&transaction;
}

static struct ref_transaction *lmdb_transaction_begin(struct strbuf *err)
{
	if (lmdb_transaction_begin_flags(err, 0))
		return NULL;
	return (struct ref_transaction *)&transaction;
}

#define MAXDEPTH 5

static const char *parse_ref_data(struct lmdb_transaction_info *info,
				  const char *refname, const char *ref_data,
				  unsigned char *sha1, int resolve_flags,
				  int *flags, int bad_name)
{
	int depth = MAXDEPTH;
	const char *buf;
	static struct strbuf refname_buffer = STRBUF_INIT;
	static struct strbuf refdata_buffer = STRBUF_INIT;
	MDB_val key, val;
	const char *ret;

	strbuf_reset(&refname_buffer);
	strbuf_reset(&refdata_buffer);

	for (;;) {
		if (--depth < 0) {
			return NULL;
		}

		if (!starts_with(ref_data, "ref:")) {
			if (get_sha1_hex(ref_data, sha1) ||
			    (ref_data[40] != '\0' && !isspace(ref_data[40]))) {
				if (flags)
					*flags |= REF_ISBROKEN;
				errno = EINVAL;
				return NULL;
			}

			if (bad_name) {
				hashclr(sha1);
				if (flags)
					*flags |= REF_ISBROKEN;
			} else if (is_null_sha1(sha1)) {
				if (flags)
					*flags |= REF_ISBROKEN;
			}
			return refname;
		}
		if (flags)
			*flags |= REF_ISSYMREF;
		buf = ref_data + 4;
		while (isspace(*buf))
			buf++;
		strbuf_addf(&refname_buffer, buf);
		refname = refname_buffer.buf;
		if (resolve_flags & RESOLVE_REF_NO_RECURSE) {
			hashclr(sha1);
			return refname;
		}
		if (check_refname_format(buf, REFNAME_ALLOW_ONELEVEL)) {
			if (flags)
				*flags |= REF_ISBROKEN;

			if (!(resolve_flags & RESOLVE_REF_ALLOW_BAD_NAME) ||
			    !refname_is_safe(buf)) {
				errno = EINVAL;
				return NULL;
			}
			bad_name = 1;
		}

		key.mv_data = (char *)refname;
		key.mv_size = strlen(refname) + 1;
		if (mdb_get_or_die(info, &key, &val)) {
			hashclr(sha1);
			if (bad_name) {
				if (flags)
					*flags |= REF_ISBROKEN;
			}
			if (resolve_flags & RESOLVE_REF_READING)
				return NULL;

			return refname;
		}
		strbuf_reset(&refdata_buffer);
		strbuf_add(&refdata_buffer, val.mv_data, val.mv_size);
		ref_data = refdata_buffer.buf;
	}
	return refname;
}

static int verify_refname_available_txn(struct lmdb_transaction_info *info,
					const char *refname,
					struct string_list *extras,
					struct string_list *skip,
					struct strbuf *err)
{
	MDB_cursor *cursor;
	MDB_val key;
	MDB_val val;
	int mdb_ret;
	size_t refname_len;
	char *search_key;
	const char *extra_refname;
	int ret = 1;
	size_t i;

	mdb_cursor_open_or_die(info, &cursor);

	refname_len = strlen(refname) + 2;
	key.mv_size = refname_len;
	search_key = xmalloc(refname_len);
	memcpy(search_key, refname, refname_len - 2);
	search_key[refname_len - 2] = '/';
	search_key[refname_len - 1] = 0;
	key.mv_data = search_key;

	/*
	 * Check for subdirs of refname: we start at refname/
	 */
	mdb_ret = mdb_cursor_get_or_die(cursor, &key, &val, MDB_SET_RANGE);

	while (!mdb_ret) {
		if (starts_with(key.mv_data, refname) &&
		    ((char*)key.mv_data)[refname_len - 2] == '/') {
			if (skip && string_list_has_string(skip, key.mv_data))
				goto next;

			strbuf_addf(err, "'%s' exists; cannot create '%s'", (char *)key.mv_data, refname);
			goto done;
		}
		break;
	next:
		mdb_ret = mdb_cursor_get_or_die(cursor, &key, &val, MDB_NEXT);
	}

	/* Check for parent dirs of refname. */
	for (i = 0; i < refname_len - 2; ++i) {
		if (search_key[i] == '/') {
			search_key[i] = 0;
			if (skip && string_list_has_string(skip, search_key)) {
				search_key[i] = '/';
				continue;
			}

			if (extras && string_list_has_string(extras, search_key)) {
				strbuf_addf(err, "cannot process '%s' and '%s' at the same time",
					    refname, search_key);
				goto done;
			}

			key.mv_data = search_key;
			key.mv_size = i + 1;
			if (!mdb_cursor_get_or_die(cursor, &key, &val, MDB_SET)) {
				strbuf_addf(err, "'%s' exists; cannot create '%s'", (char *)key.mv_data, refname);
				goto done;
			}
			search_key[i] = '/';
		}
	}

	extra_refname = find_descendant_ref(refname, extras, skip);
	if (extra_refname) {
		strbuf_addf(err,
			    "cannot process '%s' and '%s' at the same time",
			    refname, extra_refname);
		ret = 1;
	} else {
		ret = 0;
	}
done:
	mdb_cursor_close(cursor);
	free(search_key);
	return ret;
}

static const char *resolve_ref_unsafe_txn(struct lmdb_transaction_info *info,
					  const char *refname,
					  int resolve_flags,
					  unsigned char *sha1,
					  int *flags)
{
	int bad_name = 0;
	char *ref_data;
	struct MDB_val key, val;
	struct strbuf err = STRBUF_INIT;

	val.mv_size = 0;
	val.mv_data = NULL;

	if (flags)
		*flags = 0;

	if (check_refname_format(refname, REFNAME_ALLOW_ONELEVEL)) {
		if (flags)
			*flags |= REF_BAD_NAME;

		if (!(resolve_flags & RESOLVE_REF_ALLOW_BAD_NAME) ||
		    !refname_is_safe(refname)) {
			errno = EINVAL;
			return NULL;
		}
		/*
		 * dwim_ref() uses REF_ISBROKEN to distinguish between
		 * missing refs and refs that were present but invalid,
		 * to complain about the latter to stderr.
		 *
		 * We don't know whether the ref exists, so don't set
		 * REF_ISBROKEN yet.
		 */
		bad_name = 1;
	}

	key.mv_data = (void *)refname;
	key.mv_size = strlen(refname) + 1;
	if (mdb_get_or_die(info, &key, &val)) {
		if (bad_name) {
			hashclr(sha1);
			if (flags)
				*flags |= REF_ISBROKEN;
		}

		if (resolve_flags & RESOLVE_REF_READING)
			return NULL;

		if (verify_refname_available_txn(info, refname, NULL, NULL, &err)) {
			error("%s", err.buf);
			strbuf_release(&err);
			return NULL;
		}

		hashclr(sha1);
		return refname;
	}

	ref_data = val.mv_data;
	assert(ref_data[val.mv_size - 1] == 0);

	return parse_ref_data(info, refname, ref_data, sha1, resolve_flags,
			      flags, bad_name);
}

static const char *lmdb_resolve_ref_unsafe(const char *refname, int resolve_flags,
					   unsigned char *sha1, int *flags)
{
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	return resolve_ref_unsafe_txn(&transaction.info, refname,
				      resolve_flags, sha1, flags);
}

static void write_u64(char *buf, uint64_t number)
{
	int i;

	for (i = 0; i < 8; ++i)
		buf[i] = (number >> (i * 8)) & 0xff;
}

static int show_one_reflog_ent(struct strbuf *sb, each_reflog_ent_fn fn, void *cb_data)
{
	unsigned char osha1[20], nsha1[20];
	char *email_end, *message;
	unsigned long timestamp;
	int tz;

	/* old (raw) new (raw) name <email> SP time TAB msg LF */
	if (sb->len < 41 || sb->buf[sb->len - 1] != '\n' ||
	    !(email_end = strchr(sb->buf + 40, '>')) ||
	    email_end[1] != ' ' ||
	    !(timestamp = strtoul(email_end + 2, &message, 10)) ||
	    !message || message[0] != ' ' ||
	    (message[1] != '+' && message[1] != '-') ||
	    !isdigit(message[2]) || !isdigit(message[3]) ||
	    !isdigit(message[4]) || !isdigit(message[5]))
		return 0; /* corrupt? */

	hashcpy(osha1, (const unsigned char *)sb->buf);
	hashcpy(nsha1, (const unsigned char *)sb->buf + 20);

	email_end[1] = '\0';
	tz = strtol(message + 1, NULL, 10);
	if (message[6] != '\t')
		message += 6;
	else
		message += 7;
	return fn(osha1, nsha1, sb->buf + 40, timestamp, tz, message, cb_data);
}

static void format_reflog_entry(struct strbuf *buf,
				const unsigned char *old_sha1,
				const unsigned char *new_sha1,
				const char *committer, const char *msg)
{
	int len;
	int msglen;

	assert(buf->len == 0);
	strbuf_add(buf, old_sha1, 20);
	strbuf_add(buf, new_sha1, 20);
	strbuf_addstr(buf, committer);
	strbuf_addch(buf, '\n');

	len = buf->len;
	msglen = msg ? strlen(msg) : 0;
	if (msglen) {
		strbuf_grow(buf, msglen + 1);
		int copied = copy_reflog_msg(buf->buf + 40 + strlen(committer), msg) - 1;
		buf->len = len + copied;
		buf->buf[buf->len] = 0;
	}
}

static int log_ref_write(const char *refname,
			 const unsigned char *old_sha1,
			 const unsigned char *new_sha1,
			 const char *msg,
			 int flags,
			 struct strbuf *err)
{
	MDB_val key, val;
	uint64_t now = getnanotime();
	int result;
	char *log_key;
	int refname_len;
	MDB_cursor *cursor;
	struct strbuf buf = STRBUF_INIT;
	const char *timestamp;

	if (log_all_ref_updates < 0)
		log_all_ref_updates = !is_bare_repository();

	if (ref_type(refname) != REF_TYPE_NORMAL) {
		return files_log_ref_write(refname, old_sha1, new_sha1,
					   msg, flags, err);
	}

	/* it is assumed that we are in a ref transaction here */
	assert(transaction.info.txn);

	result = safe_create_reflog(refname, flags & REF_FORCE_CREATE_REFLOG, err);
	if (result)
		return result;

	/* "logs/" + refname + \0 + 8-byte timestamp for sorting and expiry. */
	refname_len = strlen(refname);
	key.mv_size = refname_len + 14;
	log_key = xcalloc(1, key.mv_size);
	sprintf(log_key, "logs/%s", refname);
	key.mv_data = log_key;

	mdb_cursor_open_or_die(&transaction.info, &cursor);

	/* check that a reflog exists */
	if (mdb_cursor_get_or_die(cursor, &key, &val, MDB_SET_RANGE) ||
	    strcmp(key.mv_data, log_key)) {
		free(log_key);
		mdb_cursor_close(cursor);
		return 0;
	}

	/* Is this a header?  We only need the header for empty reflogs */
	timestamp = (const char *)key.mv_data + refname_len + 6;
	if (ntohll(*(uint64_t *)timestamp) == 0)
		mdb_cursor_del_or_die(cursor, 0);

	key.mv_data = log_key;

	write_u64(key.mv_data + refname_len + 6, htonll(now));

	format_reflog_entry(&buf, old_sha1, new_sha1,
			    git_committer_info(0), msg);
	assert(buf.len >= 42);
	val.mv_data = buf.buf;
	val.mv_size = buf.len + 1;

	mdb_put_or_die(&transaction.info, &key, &val, 0);

	strbuf_release(&buf);
	free(log_key);
	mdb_cursor_close(cursor);
	return 0;
}

static int lmdb_verify_refname_available(const char *refname,
					 struct string_list *extras,
					 struct string_list *skip,
					 struct strbuf *err)
{
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	return verify_refname_available_txn(&transaction.info, refname, extras, skip, err);
}

static const char *check_ref(MDB_txn *txn, const char *refname,
			     const unsigned char *old_sha1,
			     unsigned char *resolved_sha1, int flags,
			     int *type_p)
{
	int mustexist = (old_sha1 && !is_null_sha1(old_sha1));
	int resolve_flags = 0;
	int type;

	if (mustexist)
		resolve_flags |= RESOLVE_REF_READING;
	if (flags & REF_DELETING) {
		resolve_flags |= RESOLVE_REF_ALLOW_BAD_NAME;
		if (flags & REF_NODEREF)
			resolve_flags |= RESOLVE_REF_NO_RECURSE;
	}

	refname = resolve_ref_unsafe(refname, resolve_flags,
				     resolved_sha1, &type);
	if (type_p)
		*type_p = type;
	if (!refname) {
		return NULL;
	}

	if (old_sha1) {
		if (flags & REF_NODEREF) {
			resolve_flags &= ~RESOLVE_REF_NO_RECURSE;
			resolve_ref_unsafe(refname, resolve_flags,
					   resolved_sha1, &type);
		}
		if (hashcmp(old_sha1, resolved_sha1)) {
			error("Ref %s is at %s but expected %s", refname,
			      sha1_to_hex(resolved_sha1), sha1_to_hex(old_sha1));

			return NULL;
		}
	}
	return refname;
}

int lmdb_transaction_create(struct ref_transaction *transaction,
			    const char *refname,
			    const unsigned char *new_sha1,
			    unsigned int flags, const char *msg,
			    struct strbuf *err)
{
	if (!new_sha1 || is_null_sha1(new_sha1))
		die("BUG: create called without valid new_sha1");
	return ref_transaction_update(transaction, refname, new_sha1,
				      null_sha1, flags, msg, err);
}

static int lmdb_transaction_delete(struct ref_transaction *transaction,
				   const char *refname,
				   const unsigned char *old_sha1,
				   unsigned int flags, const char *msg,
				   struct strbuf *err)
{
		if (old_sha1 && is_null_sha1(old_sha1))
		die("BUG: delete called with old_sha1 set to zeros");
	return ref_transaction_update(transaction, refname,
				      null_sha1, old_sha1,
				      flags, msg, err);
}

static int lmdb_transaction_verify(struct ref_transaction *transaction,
				  const char *refname,
				  const unsigned char *old_sha1,
				  unsigned int flags,
				  struct strbuf *err)
{
		if (!old_sha1)
		die("BUG: verify called with old_sha1 set to NULL");
	return ref_transaction_update(transaction, refname,
				      NULL, old_sha1,
				      flags, NULL, err);
}

static void lmdb_transaction_free_1(struct lmdb_transaction *transaction)
{
	hashmap_free(&transaction->updated_refs, 1);
	transaction->info.txn = NULL;
}

static void lmdb_transaction_free(struct ref_transaction *trans)
{
	struct lmdb_transaction *transaction = (struct lmdb_transaction *)trans;
	if (!transaction->info.txn)
		return;

	mdb_txn_abort(transaction->info.txn);
	lmdb_transaction_free_1(transaction);
	return;
}

static int lmdb_transaction_commit(struct ref_transaction *trans,
				   struct strbuf *err)
{
	struct lmdb_transaction *transaction = (struct lmdb_transaction *)trans;
	int result;

	result = mdb_txn_commit(transaction->info.txn);
	lmdb_transaction_free_1(transaction);
	return result;
}

static int lmdb_delete_reflog(const char *refname)
{
	MDB_val key, val;
	char *log_path;
	int len;
	MDB_cursor *cursor;
	int ret = 0;
	int mdb_ret;
	struct strbuf err = STRBUF_INIT;
	int in_transaction;

	if (ref_type(refname) != REF_TYPE_NORMAL)
		return files_delete_reflog(refname);

	in_transaction = in_write_transaction();

	len = strlen(refname) + 6;
	log_path = xmalloc(len);
	sprintf(log_path, "logs/%s", refname);

	key.mv_data = log_path;
	key.mv_size = len;

	if (!in_transaction)
		lmdb_transaction_begin_flags_or_die(0);

	mdb_cursor_open_or_die(&transaction.info, &cursor);

	mdb_ret = mdb_cursor_get_or_die(cursor, &key, &val, MDB_SET_RANGE);

	while (!mdb_ret) {
		if (key.mv_size < len)
			break;

		if (!starts_with(key.mv_data, log_path) || ((char*)key.mv_data)[len - 1] != 0)
			break;

		mdb_cursor_del_or_die(cursor, 0);
		mdb_ret = mdb_cursor_get_or_die(cursor, &key, &val, MDB_NEXT);
	}

	free(log_path);
	mdb_cursor_close(cursor);
	transaction.cursor = NULL;

	if (!in_transaction)
		lmdb_transaction_commit((struct ref_transaction *)&transaction, &err);
	strbuf_release(&err);
	return ret;
}

#define REF_NO_REFLOG 0x8000

static int lmdb_transaction_update(struct ref_transaction *trans,
				   const char *refname,
				   const unsigned char *new_sha1,
				   const unsigned char *old_sha1,
				   unsigned int flags, const char *msg,
				   struct strbuf *err)
{
	struct lmdb_transaction *transaction = (struct lmdb_transaction *)trans;
	const char *orig_refname = refname;
	MDB_val key, val;
	struct ref_update *update, *old;
	unsigned char resolved_sha1[20];
	size_t len;
	int type;

	if (new_sha1)
		flags |= REF_HAVE_NEW;
	if (old_sha1)
		flags |= REF_HAVE_OLD;

	if ((flags & REF_HAVE_NEW) && is_null_sha1(new_sha1))
		flags |= REF_DELETING;

	if (new_sha1 && !is_null_sha1(new_sha1) &&
	    check_refname_format(refname, REFNAME_ALLOW_ONELEVEL)) {
		strbuf_addf(err, "refusing to update ref with bad name %s",
			    refname);
		return TRANSACTION_GENERIC_ERROR;
	}

	len = strlen(orig_refname);
	update = xmalloc(sizeof(*update) + len + 1);
	update->len = len;
	strcpy(update->refname, orig_refname);
	hashmap_entry_init(update, memhash(update->refname, len));
	old = hashmap_put(&(transaction->updated_refs), update);
	if (old) {
		strbuf_addf(err, "Multiple updates for ref '%s' not allowed.",
			    orig_refname);
		free(old);
		return TRANSACTION_GENERIC_ERROR;
	}

	refname = check_ref(transaction->info.txn, orig_refname, old_sha1,
			    resolved_sha1, flags, &type);

	if (refname == NULL) {
		strbuf_addf(err, "cannot lock the ref '%s'", orig_refname);
		return TRANSACTION_GENERIC_ERROR;
	}

	if (!(flags & REF_DELETING) && is_null_sha1(resolved_sha1) &&
	    verify_refname_available_txn(&transaction->info, refname, NULL, NULL, err))
		return TRANSACTION_NAME_CONFLICT;

	if (flags & REF_NODEREF)
		refname = orig_refname;

	key.mv_size = strlen(refname) + 1;
	key.mv_data = (void *)refname;

	if ((flags & REF_HAVE_NEW) && !is_null_sha1(new_sha1)) {
		int overwriting_symref = ((type & REF_ISSYMREF) &&
					  (flags & REF_NODEREF));

		struct object *o = parse_object(new_sha1);
		if (!o) {
			strbuf_addf(err,
				    "Trying to write ref %s with nonexistent object %s",
				    refname, sha1_to_hex(new_sha1));
			return -1;
		}
		if (o->type != OBJ_COMMIT && is_branch(refname)) {
			strbuf_addf(err,
				    "Trying to write non-commit object %s to branch %s",
				    sha1_to_hex(new_sha1), refname);
			return -1;
		}

		if (!overwriting_symref
		    && !hashcmp(resolved_sha1, new_sha1)) {
			/*
			 * The reference already has the desired
			 * value, so we don't need to write it.
			 */
			flags |= REF_NO_REFLOG;
		} else {
			val.mv_size = 41;
			if (new_sha1)
				val.mv_data  = sha1_to_hex(new_sha1);
			else
				val.mv_data = sha1_to_hex(null_sha1);
			mdb_put_or_die(&transaction->info, &key, &val, 0);
		}
	}

	if (flags & REF_DELETING) {
		if (mdb_del_or_die(&transaction->info, &key, NULL)) {
			if (old_sha1 && !is_null_sha1(old_sha1)) {
				strbuf_addf(err, "No such ref %s", refname);
				return TRANSACTION_GENERIC_ERROR;
			}
		}
		lmdb_delete_reflog(orig_refname);
	} else if (!(flags & REF_NO_REFLOG)) {
		if (log_ref_write(orig_refname, resolved_sha1,
				  new_sha1 ? new_sha1 : null_sha1,
				  msg, flags, err) < 0)
			return -1;
		if (strcmp (refname, orig_refname) &&
		    log_ref_write(refname, resolved_sha1,
				  new_sha1 ? new_sha1 : null_sha1,
				  msg, flags, err) < 0)
			return -1;
	}

	return 0;
}

static int rename_reflog_ent(unsigned char *osha1, unsigned char *nsha1,
			     const char *email, unsigned long timestamp, int tz,
			     const char *message, void *cb_data)
{

	const char *newrefname = cb_data;
	MDB_val key, new_key, val;

	assert(transaction.cursor);

	if (mdb_cursor_get_or_die(transaction.cursor, &key, &val, MDB_GET_CURRENT))
		die("renaming ref: mdb_cursor_get failed to get current");

	new_key.mv_size = strlen(newrefname) + 5 + 1 + 8;
	new_key.mv_data = xmalloc(new_key.mv_size);
	strcpy(new_key.mv_data, "logs/");
	strcpy(new_key.mv_data + 5, newrefname);
	memcpy(new_key.mv_data + new_key.mv_size - 8, key.mv_data + key.mv_size - 8, 8);
	mdb_put_or_die(&transaction.info, &new_key, &val, 0);
	mdb_cursor_del_or_die(transaction.cursor, 0);
	free(new_key.mv_data);
	return 0;
}

static int lmdb_rename_ref(const char *oldref, const char *newref, const char *logmsg)
{
	unsigned char orig_sha1[20];
	int flag = 0;
	int log = reflog_exists(oldref);
	const char *symref = NULL;
	struct strbuf err = STRBUF_INIT;
	struct ref_transaction *ref_transaction;

	assert(!in_write_transaction());

	if (!strcmp(oldref, newref))
		return 0;

	ref_transaction = lmdb_transaction_begin_flags_or_die(0);

	symref = resolve_ref_unsafe(oldref, RESOLVE_REF_READING,
				    orig_sha1, &flag);
	if (flag & REF_ISSYMREF)
		return error("refname %s is a symbolic ref, renaming it is not supported",
			oldref);
	if (!symref)
		return error("refname %s not found", oldref);

	if (!rename_ref_available(oldref, newref))
		return 1;

	if (log) {
		struct strbuf old_log_sentinel = STRBUF_INIT;
		MDB_val key;
		int log_all;

		log_all = log_all_ref_updates;
		log_all_ref_updates = 1;
		if (safe_create_reflog(newref, 0, &err)) {
			error("can't create reflog for %s: %s", newref, err.buf);
			strbuf_release(&err);
			return 1;
		}
		log_all_ref_updates = log_all;

		for_each_reflog_ent(oldref, rename_reflog_ent, (void *)newref);
		strbuf_addf(&old_log_sentinel, "logs/%sxxxxxxxx", oldref);
		memset(old_log_sentinel.buf + old_log_sentinel.len - 8, 0, 8);

		key.mv_size = old_log_sentinel.len;
		key.mv_data = old_log_sentinel.buf;
		/* It's OK if the old reflog is missing */
		mdb_del_or_die(&transaction.info, &key, NULL);

		strbuf_release(&old_log_sentinel);
	}

	if (ref_transaction_delete(ref_transaction, oldref,
		orig_sha1, REF_NODEREF, NULL, &err)) {
		error("unable to delete old %s", oldref);
		return 1;
	}

	if (lmdb_transaction_update(ref_transaction, newref, orig_sha1, NULL,
				    0, logmsg, &err)) {
		error("%s", err.buf);
		strbuf_release(&err);
		return 1;
	}

	if (lmdb_transaction_commit(ref_transaction, &err)) {
		error("%s", err.buf);
		strbuf_release(&err);
		return 1;
	}

	return 0;

}

static int lmdb_for_each_reflog_ent_order(const char *refname,
					  each_reflog_ent_fn fn,
					  void *cb_data, int reverse)
{
	MDB_val key, val;
	char *search_key;
	char *log_path;
	int len;
	MDB_cursor *cursor;
	int ret = 0;
	struct strbuf sb = STRBUF_INIT;
	enum MDB_cursor_op direction = reverse ? MDB_PREV : MDB_NEXT;
	uint64_t zero = 0ULL;

	len = strlen(refname) + 6;
	log_path = xmalloc(len);
	search_key = xmalloc(len + 1);
	sprintf(log_path, "logs/%s", refname);
	strcpy(search_key, log_path);

	if (reverse) {
		/*
		 * For a reverse search, start at the key
		 * lexicographically after the searched-for key.
		 * That's the one with \001 appended to the key.
		 */

		search_key[len - 1] = 1;
		search_key[len] = 0;
		key.mv_size = len + 1;
	} else {
		key.mv_size = len;
	}

	key.mv_data = search_key;

	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);

	mdb_cursor_open_or_die(&transaction.info, &cursor);

	transaction.cursor = cursor;

	/*
	 * MDB's cursor API requires that the first mdb_cursor_get be
	 * called with MDB_SET_RANGE.  For reverse searches, this will
	 * give us the entry one-past the entry we're looking for, so
	 * we should jump back using MDB_PREV.
	 */
	mdb_cursor_get_or_die(cursor, &key, &val, MDB_SET_RANGE);
	if (direction == MDB_PREV)
		mdb_cursor_get_or_die(cursor, &key, &val, direction);

	do {
		if (key.mv_size < len)
			break;

		if (!starts_with(key.mv_data, log_path) || ((char *)key.mv_data)[len - 1] != 0)
			break;

		if (!memcmp(&zero, ((char *)key.mv_data) + key.mv_size - 8, 8))
			continue;

		assert(val.mv_size != 0);

		strbuf_add(&sb, val.mv_data, val.mv_size - 1);
		ret = show_one_reflog_ent(&sb, fn, cb_data);
		if (ret)
			break;

		strbuf_reset(&sb);
	} while (!mdb_cursor_get_or_die(cursor, &key, &val, direction));

	strbuf_release(&sb);
	free(log_path);
	free(search_key);
	mdb_cursor_close(cursor);
	return ret;
}

static int lmdb_for_each_reflog_ent(const char *refname,
				    each_reflog_ent_fn fn,
				    void *cb_data)
{
	if (ref_type(refname) != REF_TYPE_NORMAL)
		return files_for_each_reflog_ent(refname, fn, cb_data);
	return lmdb_for_each_reflog_ent_order(refname, fn, cb_data, 0);
}

static int lmdb_for_each_reflog_ent_reverse(const char *refname,
					    each_reflog_ent_fn fn,
					    void *cb_data)
{
	if (ref_type(refname) != REF_TYPE_NORMAL)
		return files_for_each_reflog_ent_reverse(refname, fn, cb_data);
	return lmdb_for_each_reflog_ent_order(refname, fn, cb_data, 1);
}

static int lmdb_reflog_exists(const char *refname)
{
	MDB_val key, val;
	char *log_path;
	int len;
	MDB_cursor *cursor;
	int ret = 1;

	if (ref_type(refname) != REF_TYPE_NORMAL)
		return files_reflog_exists(refname);

	len = strlen(refname) + 6;
	log_path = xmalloc(len);
	sprintf(log_path, "logs/%s", refname);

	key.mv_data = log_path;
	key.mv_size = len;

	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	mdb_cursor_open_or_die(&transaction.info, &cursor);

	if (mdb_cursor_get_or_die(cursor, &key, &val, MDB_SET_RANGE)) {
		ret = 0;
	} else if (!starts_with(key.mv_data, log_path)) {
		ret = 0;
	}

	free(log_path);
	mdb_cursor_close(cursor);

	return ret;
}

struct wrapped_each_ref_fn {
	each_ref_fn *fn;
	void *cb_data;
};

static int check_reflog(const char *refname,
			const struct object_id *oid, int flags, void *cb_data)
{
	struct wrapped_each_ref_fn *wrapped = cb_data;

	if (reflog_exists(refname))
		return wrapped->fn(refname, oid, 0, wrapped->cb_data);

	return 0;
}

static int lmdb_for_each_reflog(each_ref_fn fn, void *cb_data)
{
	struct wrapped_each_ref_fn wrapped = {fn, cb_data};
	int result = head_ref(fn, cb_data);
	if (result)
	    return result;
	return for_each_ref(check_reflog, &wrapped);
}

static int lmdb_create_reflog(const char *refname, int force_create, struct strbuf *err)
{
	/*
	 * We mark that there is a reflog by creating a key of the
	 * form logs/$refname followed by nine \0 (one for
	 * string-termination, 8 in lieu of a timestamp), with an empty
	 * value.
	 */

	int in_transaction = in_write_transaction();
	MDB_val key, val;

	if (!force_create && !should_autocreate_reflog(refname))
		return 0;

	if (!in_transaction)
		lmdb_transaction_begin_flags_or_die(0);

	key.mv_size = strlen(refname) + 5 + 1 + 8;
	key.mv_data = xcalloc(1, key.mv_size);
	sprintf((char *)key.mv_data, "logs/%s", refname);
	val.mv_size = 0;
	val.mv_data = NULL;
	mdb_put_or_die(&transaction.info, &key, &val, 0);

	free(key.mv_data);
	if (!in_transaction)
		return lmdb_transaction_commit(
			(struct ref_transaction *)&transaction, err);
	return 0;
}

struct expire_reflog_cb {
	unsigned int flags;
	reflog_expiry_should_prune_fn *should_prune_fn;
	void *policy_cb;
	unsigned char last_kept_sha1[20];
};

static int expire_reflog_ent(unsigned char *osha1, unsigned char *nsha1,
			     const char *email, unsigned long timestamp, int tz,
			     const char *message, void *cb_data)
{
	struct expire_reflog_cb *cb = cb_data;
	struct expire_reflog_policy_cb *policy_cb = cb->policy_cb;

	if (cb->flags & EXPIRE_REFLOGS_REWRITE)
		osha1 = cb->last_kept_sha1;

	if ((*cb->should_prune_fn)(osha1, nsha1, email, timestamp, tz,
				   message, policy_cb)) {
		if (cb->flags & EXPIRE_REFLOGS_DRY_RUN)
			printf("would prune %s", message);
		else {
			if (cb->flags & EXPIRE_REFLOGS_VERBOSE)
				printf("prune %s", message);

			mdb_cursor_del_or_die(transaction.cursor, 0);
		}
	} else {
		hashcpy(cb->last_kept_sha1, nsha1);
		if (cb->flags & EXPIRE_REFLOGS_VERBOSE)
			printf("keep %s", message);
	}
	return 0;
}

static int write_ref(const char *refname, const unsigned char *sha1)
{
	struct strbuf err = STRBUF_INIT;
	struct ref_transaction *transaction;

	transaction = lmdb_transaction_begin(&err);
	if (!transaction) {
		error("%s", err.buf);
		strbuf_release(&err);
		return -1;
	}

	if (lmdb_transaction_update(transaction, refname, sha1, NULL,
				    REF_NO_REFLOG, NULL, &err)) {
		error("%s", err.buf);
		strbuf_release(&err);
		return -1;
	}

	if (lmdb_transaction_commit(transaction, &err)) {
		error("%s", err.buf);
		strbuf_release(&err);
		return -1;
	}

	return 0;
}

int lmdb_reflog_expire(const char *refname, const unsigned char *sha1,
		       unsigned int flags,
		       reflog_expiry_prepare_fn prepare_fn,
		       reflog_expiry_should_prune_fn should_prune_fn,
		       reflog_expiry_cleanup_fn cleanup_fn,
		       void *policy_cb_data)
{
	struct expire_reflog_cb cb;
	int dry_run = flags & EXPIRE_REFLOGS_DRY_RUN;
	int status = 0;
	struct strbuf err = STRBUF_INIT;
	unsigned char resolved_sha1[20];
	int type;

	if (ref_type(refname) != REF_TYPE_NORMAL)
		return files_reflog_expire(refname, sha1, flags, prepare_fn,
					   should_prune_fn, cleanup_fn,
					   policy_cb_data);

	memset(&cb, 0, sizeof(cb));
	cb.flags = flags;
	cb.policy_cb = policy_cb_data;
	cb.should_prune_fn = should_prune_fn;

	lmdb_transaction_begin_flags_or_die(dry_run ? MDB_RDONLY : 0);

	check_ref(transaction.info.txn, refname, sha1,
		  resolved_sha1, 0, &type);

	(*prepare_fn)(refname, sha1, cb.policy_cb);
	lmdb_for_each_reflog_ent(refname, expire_reflog_ent, &cb);
	(*cleanup_fn)(cb.policy_cb);

	if (!dry_run) {
		/*
		 * It doesn't make sense to adjust a reference pointed
		 * to by a symbolic ref based on expiring entries in
		 * the symbolic reference's reflog. Nor can we update
		 * a reference if there are no remaining reflog
		 * entries.
		 */
		int update = (flags & EXPIRE_REFLOGS_UPDATE_REF) &&
			!(type & REF_ISSYMREF) &&
			!is_null_sha1(cb.last_kept_sha1);

		if (lmdb_transaction_commit(
			    (struct ref_transaction *)&transaction, &err)) {
			status |= error("couldn't write logs/%s: %s", refname,
					err.buf);
			strbuf_release(&err);
		} else if (update &&
			   write_ref(refname, cb.last_kept_sha1)) {
			status |= error("couldn't set %s",
					refname);
		}
	}
	return status;
}

static int lmdb_pack_refs(unsigned int flags)
{
	/* This concept does not exist in this backend. */
	return 0;
}

static int lmdb_peel_ref(const char *refname, unsigned char *sha1)
{
	int flag;
	unsigned char base[20];

	if (read_ref_full(refname, RESOLVE_REF_READING, base, &flag))
		return -1;

	return peel_object(base, sha1);
}

static int lmdb_create_symref(struct ref_transaction *trans,
			      const char *ref_target,
			      const char *refs_heads_master,
			      const char *logmsg)
{

	struct strbuf err = STRBUF_INIT;
	unsigned char old_sha1[20], new_sha1[20];
	MDB_val key, val;
	char *valdata;
	struct lmdb_transaction *transaction = (struct lmdb_transaction *)trans;

	if (logmsg && read_ref(ref_target, old_sha1))
		hashclr(old_sha1);

	key.mv_size = strlen(ref_target) + 1;
	key.mv_data = xstrdup(ref_target);

	val.mv_size = strlen(refs_heads_master) + 1 + 5;
	valdata = xmalloc(val.mv_size);
	sprintf(valdata, "ref: %s", refs_heads_master);
	val.mv_data = valdata;

	mdb_put_or_die(&transaction->info, &key, &val, 0);

	if (logmsg && !read_ref(refs_heads_master, new_sha1) &&
	    log_ref_write(ref_target, old_sha1, new_sha1, logmsg, 0, &err)) {
		error("log_ref_write failed: %s", err.buf);
		strbuf_release(&err);
	}

	free(key.mv_data);
	free(valdata);

	return 0;
}

MDB_env *submodule_txn_begin(struct lmdb_transaction_info *info)
{
	int ret;
	MDB_env *submodule_env = NULL;
	struct strbuf path = STRBUF_INIT;

	strbuf_git_path_submodule(&path, info->submodule, "refdb");

	if (!is_directory(path.buf))
		goto done;

	mkdir(path.buf, 0775);

	init_env(&submodule_env, path.buf);

	if ((ret = mdb_txn_begin(submodule_env, NULL, MDB_RDONLY, &info->txn)) != MDB_SUCCESS) {
		die("mdb_txn_begin failed: %s", mdb_strerror(ret));

	}
	if ((ret = mdb_dbi_open(info->txn, NULL, 0, &info->dbi)) != MDB_SUCCESS) {
		die("mdb_txn_open failed: %s", mdb_strerror(ret));
	}

done:
	strbuf_release(&path);
	return submodule_env;
}

static int lmdb_resolve_gitlink_ref(const char *submodule, const char *refname,
				     unsigned char *sha1)
{
	struct lmdb_transaction_info info;
	MDB_env *submodule_env;
	int result;

	info.txn = NULL;
	info.submodule = submodule;
	submodule_env = submodule_txn_begin(&info);
	if (!submodule_env)
		return -1;
	result = !resolve_ref_unsafe_txn(&info, refname,
					 RESOLVE_REF_READING, sha1, NULL);

	mdb_txn_abort(info.txn);
	mdb_env_close(submodule_env);
	return result ? -1 : 0;
}

static int do_head_ref(const char *submodule, each_ref_fn fn, void *cb_data)
{
	struct object_id oid;
	int flag;

	if (submodule) {
		if (resolve_gitlink_ref(submodule, "HEAD", oid.hash) == 0)
			return fn("HEAD", &oid, 0, cb_data);

		return 0;
	}

	if (!read_ref_full("HEAD", RESOLVE_REF_READING, oid.hash, &flag))
		return fn("HEAD", &oid, flag, cb_data);

	return 0;
}

static int lmdb_head_ref(each_ref_fn fn, void *cb_data)
{
	return do_head_ref(NULL, fn, cb_data);
}

static int lmdb_head_ref_submodule(const char *submodule, each_ref_fn fn,
				    void *cb_data)
{
	return do_head_ref(submodule, fn, cb_data);
}

/*
 * Call fn for each reference for which the refname begins with base.
 * If trim is non-zero, then trim that many characters off the
 * beginning of each refname before passing the refname to fn.  flags
 * can be DO_FOR_EACH_INCLUDE_BROKEN to include broken references in
 * the iteration.  If fn ever returns a non-zero value, stop the
 * iteration and return that value; otherwise, return 0.
 */
static int do_for_each_ref(struct lmdb_transaction_info *info,
			   const char *base, each_ref_fn fn, int trim,
			   int flags, void *cb_data)
{

	MDB_val key, val;
	MDB_cursor *cursor;
	int baselen;
	char *search_key;
	int retval;
	int mdb_ret;

	retval = do_for_each_per_worktree_ref(info->submodule, base, fn, trim,
					      flags, cb_data);
	if (retval)
		return retval;

	if (ref_paranoia < 0)
		ref_paranoia = git_env_bool("GIT_REF_PARANOIA", 0);
	if (ref_paranoia)
		flags |= DO_FOR_EACH_INCLUDE_BROKEN;

	if (!base || !*base) {
		base = "refs/";
		trim = 0;
	}

	baselen = strlen(base);
	search_key = xmalloc(baselen + 1);
	strcpy(search_key, base);
	key.mv_size = baselen + 1;
	key.mv_data = search_key;

	mdb_cursor_open_or_die(info, &cursor);

	mdb_ret = mdb_cursor_get_or_die(cursor, &key, &val, MDB_SET_RANGE);

	while (!mdb_ret) {
		struct object_id oid;
		int parsed_flags = 0;

		if (memcmp(key.mv_data, base, baselen))
			break;

		parse_ref_data(info, key.mv_data + (trim ? baselen : 0),
			       val.mv_data, oid.hash, 0, &parsed_flags, 0);

		if (flags & DO_FOR_EACH_INCLUDE_BROKEN ||
		    (!(parsed_flags & REF_ISBROKEN) &&
		     has_sha1_file(oid.hash))) {
			retval = fn(key.mv_data + (trim ? baselen : 0), &oid, parsed_flags, cb_data);
			if (retval)
				break;
		}

		mdb_ret = mdb_cursor_get_or_die(cursor, &key, &val, MDB_NEXT);
	}

	mdb_cursor_close(cursor);
	free(search_key);

	return retval;
}

static int lmdb_for_each_ref(each_ref_fn fn, void *cb_data)
{
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	return do_for_each_ref(&transaction.info, "", fn, 0, 0, cb_data);
}

static int lmdb_for_each_ref_submodule(const char *submodule, each_ref_fn fn,
					void *cb_data)
{
	struct lmdb_transaction_info info;
	MDB_env *submodule_env;
	int result;

	if (!submodule)
		return for_each_ref(fn, cb_data);

	info.txn = NULL;
	info.submodule = submodule;

	submodule_env = submodule_txn_begin(&info);
	if (!submodule_env)
		return 0;
	result = do_for_each_ref(&info, "", fn, 0, 0, cb_data);
	mdb_txn_abort(info.txn);
	mdb_env_close(submodule_env);
	return result;
}

static int lmdb_for_each_ref_in(const char *prefix, each_ref_fn fn,
				 void *cb_data)
{
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	return do_for_each_ref(&transaction.info, prefix, fn, strlen(prefix),
			       0, cb_data);
}

static int lmdb_for_each_fullref_in(const char *prefix, each_ref_fn fn,
				     void *cb_data, unsigned int broken)
{
	unsigned int flag = 0;

	if (broken)
		flag = DO_FOR_EACH_INCLUDE_BROKEN;
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	return do_for_each_ref(&transaction.info, prefix, fn, 0, flag, cb_data);
}

static int lmdb_for_each_ref_in_submodule(const char *submodule,
					   const char *prefix,
					   each_ref_fn fn, void *cb_data)
{
	struct lmdb_transaction_info info = {NULL};
	MDB_env *submodule_env;
	int result;

	if (!submodule)
		return for_each_ref_in(prefix, fn, cb_data);

	info.submodule = submodule;
	submodule_env = submodule_txn_begin(&info);
	if (!submodule_env)
		return 0;
	result = do_for_each_ref(&info, prefix, fn,
				 strlen(prefix), 0, cb_data);
	mdb_txn_abort(info.txn);
	mdb_env_close(submodule_env);
	return result;
}

static int lmdb_for_each_replace_ref(each_ref_fn fn, void *cb_data)
{
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	return do_for_each_ref(&transaction.info, git_replace_ref_base, fn,
			       strlen(git_replace_ref_base), 0, cb_data);
}

static int lmdb_for_each_reftype_fullpath(each_ref_fn fn, char *type, unsigned int broken, void *cb_data)
{
	unsigned int flag = 0;

	if (broken)
		flag = DO_FOR_EACH_INCLUDE_BROKEN;
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	return do_for_each_ref(&transaction.info, type, fn, 0, flag, cb_data);
}

static int lmdb_for_each_namespaced_ref(each_ref_fn fn, void *cb_data)
{
	struct strbuf buf = STRBUF_INIT;
	int ret;
	strbuf_addf(&buf, "%srefs/", get_git_namespace());
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	ret = do_for_each_ref(&transaction.info, buf.buf, fn, 0, 0, cb_data);
	strbuf_release(&buf);
	return ret;
}

static int lmdb_for_each_rawref(each_ref_fn fn, void *cb_data)
{
	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	return do_for_each_ref(&transaction.info, "", fn, 0,
			       DO_FOR_EACH_INCLUDE_BROKEN, cb_data);
}

/* For testing only! */
int test_refdb_raw_read(const char *key)
{
	MDB_val key_val, val;
	char *keydup;
	int ret;

	lmdb_transaction_begin_flags_or_die(MDB_RDONLY);
	keydup = xstrdup(key);
	key_val.mv_data = keydup;
	key_val.mv_size = strlen(key) + 1;

	ret = mdb_get_or_die(&transaction.info, &key_val, &val);
	free(keydup);
	switch (ret) {
	case 0:
		printf("%s\n", (char *)val.mv_data);
		return 0;
	case MDB_NOTFOUND:
		fprintf(stderr, "%s not found\n", key);
		return 1;
	default:
		return 2;
	}
}

/* For testing only! */
void test_refdb_raw_write(const char *key, const char *value)
{
	MDB_val key_val, val;
	char *keydup, *valdup;
	struct strbuf err = STRBUF_INIT;

	lmdb_transaction_begin_flags_or_die(0);

	keydup = xstrdup(key);
	key_val.mv_data = keydup;
	key_val.mv_size = strlen(key) + 1;

	valdup = xstrdup(value);
	val.mv_data = valdup;
	val.mv_size = strlen(value) + 1;

	mdb_put_or_die(&transaction.info, &key_val, &val, 0);
	assert(lmdb_transaction_commit((struct ref_transaction *)&transaction,
				       &err) == 0);

	free(keydup);
	free(valdup);
}

/* For testing only! */
int test_refdb_raw_delete(const char *key)
{
	MDB_val key_val;
	char *keydup;
	int ret;

	struct strbuf err = STRBUF_INIT;

	lmdb_transaction_begin_flags_or_die(0);
	keydup = xstrdup(key);
	key_val.mv_data = keydup;
	key_val.mv_size = strlen(key) + 1;

	ret = mdb_del_or_die(&transaction.info, &key_val, NULL);

	assert(lmdb_transaction_commit((struct lmdb_transaction *)&transaction,
				       &err) == 0);

	free(keydup);
	return ret;
}

static int print_raw_reflog_ent(unsigned char *osha1, unsigned char *nsha1,
				const char *email, unsigned long timestamp,
				int tz, const char *message, void *cb_data)
{
	int *any = cb_data;
	*any = 1;

	if (*message != '\n')
		printf("%s %s %s %lu %+05d\t%s", sha1_to_hex(osha1),
		       sha1_to_hex(nsha1),
		       email, timestamp, tz, message);
	else
		printf("%s %s %s %lu %+05d\n", sha1_to_hex(osha1),
		       sha1_to_hex(nsha1),
		       email, timestamp, tz);
	return 0;
}

/* For testing only! */
int test_refdb_raw_reflog(const char *refname)
{
	MDB_val key, val;
	char *search_key;
	char *log_path;
	int len;
	MDB_cursor *cursor;
	int result = 1;
	int any = 0;

	for_each_reflog_ent(refname, print_raw_reflog_ent, &any);

	return !any;
}

/* For testing only! */
void test_refdb_raw_delete_reflog(char *refname)
{
	MDB_val key, val;
	int mdb_ret;
	char *search_key;
	MDB_cursor *cursor;
	int len;
	struct strbuf err = STRBUF_INIT;

	if (refname) {
		len = strlen(refname) + 5 + 1; /* logs/ + 0*/
		search_key = xmalloc(len);
		sprintf(search_key, "logs/%s", refname);
	} else {
		len = 6; /* logs/ + 0*/
		search_key = xstrdup("logs/");
	}
	key.mv_data = search_key;
	key.mv_size = len;

	lmdb_transaction_begin_flags_or_die(0);

	mdb_cursor_open_or_die(&transaction.info, &cursor);

	mdb_ret = mdb_cursor_get_or_die(cursor, &key, &val, MDB_SET_RANGE);
	while (!mdb_ret) {
		if (!starts_with(key.mv_data, search_key))
			break;
		if (refname && ((char *)val.mv_data)[len - 1] == 0)
			break;

		mdb_cursor_del_or_die(cursor, 0);
		mdb_ret = mdb_cursor_get_or_die(cursor, &key, &val, MDB_NEXT);
	}

	free(search_key);
	mdb_cursor_close(cursor);

	assert(lmdb_transaction_commit(
		       (struct ref_transaction *)&transaction, &err) == 0);
	return;
}

static void format_lmdb_reflog_ent(struct strbuf *dst, struct strbuf *src)
{
	unsigned char osha1[20], nsha1[20];
	const char *msg;

	get_sha1_hex(src->buf, osha1);
	get_sha1_hex(src->buf + 41, nsha1);

	msg = strchr(src->buf + 82, '\n');
	assert(msg);
	msg += 1;

	format_reflog_entry(dst, osha1, nsha1, src->buf + 82, msg);
}

/* For testing only! */
void test_refdb_raw_append_reflog(const char *refname)
{
	struct strbuf input = STRBUF_INIT;
	uint64_t now = getnanotime();
	MDB_val key, val;
	struct strbuf err = STRBUF_INIT;

	key.mv_size = strlen(refname) + 14;
	key.mv_data = xcalloc(1, key.mv_size);
	sprintf(key.mv_data, "logs/%s", refname);

	lmdb_transaction_begin_flags_or_die(0);

	/* We do not remove the header here, because this is just for
	 * tests, so it's OK to be a bit inefficient */

	while (strbuf_getwholeline(&input, stdin, '\n') != EOF) {
		struct strbuf sb = STRBUF_INIT;

		/* "logs/" + \0 + 8-byte timestamp for sorting and expiry */
		write_u64(key.mv_data + key.mv_size - 8, htonll(now++));

		/* Convert the input from files-reflog format to
		 * lmdb-reflog-format */

		format_lmdb_reflog_ent(&sb, &input);
		val.mv_data = sb.buf;
		val.mv_size = sb.len + 1;
		mdb_put_or_die(&transaction.info, &key, &val, 0);
		strbuf_reset(&sb);
		input.len = 0;
	}

	assert(lmdb_transaction_commit(&transaction, &err) == 0);
	free(key.mv_data);
}

struct ref_be refs_be_lmdb = {
	NULL,
	"lmdb",
	lmdb_init_backend,
	lmdb_initdb,
	lmdb_transaction_begin,
	lmdb_transaction_update,
	lmdb_transaction_create,
	lmdb_transaction_delete,
	lmdb_transaction_verify,
	lmdb_transaction_commit,
	lmdb_transaction_commit, /* initial commit */
	lmdb_transaction_free,
	lmdb_rename_ref,
	lmdb_for_each_reflog_ent,
	lmdb_for_each_reflog_ent_reverse,
	lmdb_for_each_reflog,
	lmdb_reflog_exists,
	lmdb_create_reflog,
	lmdb_delete_reflog,
	lmdb_reflog_expire,
	lmdb_resolve_ref_unsafe,
	lmdb_verify_refname_available,
	lmdb_pack_refs,
	lmdb_peel_ref,
	lmdb_create_symref,
	lmdb_resolve_gitlink_ref,
	lmdb_head_ref,
	lmdb_head_ref_submodule,
	lmdb_for_each_ref,
	lmdb_for_each_ref_submodule,
	lmdb_for_each_ref_in,
	lmdb_for_each_fullref_in,
	lmdb_for_each_ref_in_submodule,
	lmdb_for_each_rawref,
	lmdb_for_each_namespaced_ref,
	lmdb_for_each_replace_ref,
	lmdb_for_each_reftype_fullpath,
};