/*****************************************************************************

Copyright (c) 2017, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/** @file dict/dict0dd.cc
Data dictionary interface */

#include <current_thd.h>

#include "dict0dd.h"
#include "dict0dict.h"
#include "dict0crea.h"
#include "dict0priv.h"
#include <dd/properties.h>
#include "dict0mem.h"
#include "dict0stats.h"
#include "rem0rec.h"
#include "data0type.h"
#include "mach0data.h"
#include "dict0dict.h"
#include "fts0priv.h"
#include "ut0crc32.h"
#include "srv0start.h"
#include "sql_table.h"
#include "sql_base.h"
#include "ha_innodb.h"
#include "derror.h"
#include "fts0plugin.h"
#include <bitset>


#ifdef UNIV_DEBUG

/** Verify a metadata lock.
@param[in,out]	thd	current thread
@param[in]	db	schema name
@param[in]	table	table name
@retval	false if acquired
@retval	true if lock is there */
static
bool
dd_mdl_verify(
	THD*			thd,
	const char*		db,
	char*			table)
{
	bool	ret = false;

	/* If InnoDB acquires MDL lock on partition table, it always
	acquires on its parent table name */
	char*   is_part = NULL;
#ifdef _WIN32
                is_part = strstr(table, "#p#");
#else
                is_part = strstr(table, "#P#");
#endif /* _WIN32 */

	if (is_part) {
		*is_part ='\0';
	}

	ret = dd::has_shared_table_mdl(thd, db, table);

	if (is_part) {
		*is_part = '#';
	}

	return(ret);
}

#endif /* UNIV_DEBUG */

/** Release a metadata lock.
@param[in,out]	thd	current thread
@param[in,out]	mdl	metadata lock */
void
dd_mdl_release(
	THD*		thd,
	MDL_ticket**	mdl)
{
	ut_ad(*mdl != nullptr);
	dd::release_mdl(thd, *mdl);
	*mdl = nullptr;
}

/** Instantiate an InnoDB in-memory table metadata (dict_table_t)
based on a Global DD object.
@param[in,out]	client		data dictionary client
@param[in]	dd_table	Global DD table object
@param[in]	dd_part		Global DD partition or subpartition, or NULL
@param[in]	tbl_name	table name, or NULL if not known
@param[out]	table		InnoDB table (NULL if not found or loadable)
@param[in]	skip_mdl	whether meta-data locking is skipped
@return	error code
@retval	0	on success */
int
dd_table_open_on_dd_obj(
	dd::cache::Dictionary_client*	client,
	const dd::Table&		dd_table,
	const dd::Partition*		dd_part,
	const char*			tbl_name,
	dict_table_t*&			table,
	bool				skip_mdl,
	THD*				thd)
{
	ut_ad(dd_table.is_persistent());
	ut_ad(dd_part == nullptr || &dd_part->table() == &dd_table);
	ut_ad(dd_part == nullptr
	      || dd_table.se_private_id() == dd::INVALID_OBJECT_ID);
	ut_ad(dd_part == nullptr
	      || dd_table.partition_type() != dd::Table::PT_NONE);
	ut_ad(dd_part == nullptr
	      || dd_part->level() == (dd_part->parent() != nullptr));
	ut_ad(dd_part == nullptr
	      || ((dd_part->table().subpartition_type() != dd::Table::ST_NONE)
		  == (dd_part->parent() != nullptr)));
	ut_ad(dd_part == nullptr
	      || dd_part->parent() == nullptr
	      || dd_part->parent()->level() == 0);
#ifdef UNIV_DEBUG
	/* If this is a internal temporary table, it's impossible
	to verify the MDL against the table name, because both the
	database name and table name may be invalid for MDL */
	if (tbl_name && !row_is_mysql_tmp_table_name(tbl_name)) {
		char	db_buf[NAME_LEN + 1];
		char	tbl_buf[NAME_LEN + 1];

		innobase_parse_tbl_name(tbl_name, db_buf, tbl_buf, NULL);
		if (dd_part == NULL) {
			ut_ad(innobase_strcasecmp(dd_table.name().c_str(),
						  tbl_buf) == 0);
		} else {
			ut_ad(strncmp(dd_table.name().c_str(), tbl_buf,
				      dd_table.name().size()) == 0);
		}

		ut_ad(skip_mdl
		      || dd_mdl_verify(thd, db_buf, tbl_buf));
	}
#endif /* UNIV_DEBUG */

	int			error		= 0;
	const table_id_t	table_id	= dd_part == nullptr
		? dd_table.se_private_id()
		: dd_part->se_private_id();
	const ulint		fold		= ut_fold_ull(table_id);

	ut_ad(table_id != dd::INVALID_OBJECT_ID);

	mutex_enter(&dict_sys->mutex);

	HASH_SEARCH(id_hash, dict_sys->table_id_hash, fold,
                    dict_table_t*, table, ut_ad(table->cached),
                    table->id == table_id);

	if (table != nullptr) {
		table->acquire();
	}

	mutex_exit(&dict_sys->mutex);

	if (table || error) {
		return(error);
	}

	TABLE_SHARE		ts;
	dd::Schema*		schema;
	const char*		table_cache_key;
	size_t			table_cache_key_len;

	if (tbl_name != nullptr) {
		schema = nullptr;
		table_cache_key = tbl_name;
		table_cache_key_len = dict_get_db_name_len(tbl_name);
	} else {
		error = client->acquire_uncached<dd::Schema>(
			dd_table.schema_id(), &schema);

		if (error) {
			return(error);
		}

		table_cache_key = schema->name().c_str();
		table_cache_key_len = schema->name().size();
	}

	init_tmp_table_share(thd,
			     &ts, table_cache_key, table_cache_key_len,
			     dd_table.name().c_str(), ""/* file name */,
			     nullptr);

	error = open_table_def(thd, &ts, false, &dd_table);

	if (!error) {
		TABLE	td;

		error = open_table_from_share(thd, &ts,
					      dd_table.name().c_str(),
					      0, OPEN_FRM_FILE_ONLY, 0,
					      &td, false, &dd_table);
		if (!error) {
			char		tmp_name[2 * (NAME_LEN + 1)];
			const char*	tab_namep;

			if (tbl_name) {
				tab_namep = tbl_name;
			} else {
				snprintf(tmp_name, sizeof tmp_name,
					 "%s/%s", schema->name().c_str(),
					 dd_table.name().c_str());
				tab_namep = tmp_name;
			}

			if (dd_part == NULL) {
				table = dd_open_table(
					client, &td, tab_namep, table,
					&dd_table, skip_mdl, thd);
			} else {
				table = dd_open_table(
					client, &td, tab_namep, table,
					dd_part, skip_mdl, thd);
			}
		}

		closefrm(&td, false);
	}

	free_table_share(&ts);

	return(error);
}

/** Load an InnoDB table definition by InnoDB table ID.
@param[in,out]	thd		current thread
@param[in,out]	mdl		metadata lock;
nullptr if we are resurrecting table IX locks in recovery
@param[in]	tbl_name	table name for already granted MDL,
or nullptr if mdl==nullptr or *mdl==nullptr
@param[in]	table_id	InnoDB table or partition ID
@return	InnoDB table
@retval	nullptr	if the table is not found, or there was an error */
static
dict_table_t*
dd_table_open_on_id_low(
	THD*			thd,
	MDL_ticket**		mdl,
	const char*		tbl_name,
	table_id_t		table_id)
{
	ut_ad(thd == nullptr || thd == current_thd);
#ifdef UNIV_DEBUG
	btrsea_sync_check	check(false);
	ut_ad(!sync_check_iterate(check));
#endif
	ut_ad(!srv_is_being_shutdown);

	if (thd == nullptr) {
		ut_ad(mdl == nullptr);
		thd = current_thd;
	}

#ifdef UNIV_DEBUG
	char	db_buf[NAME_LEN + 1];
	char	tbl_buf[NAME_LEN + 1];

	if (tbl_name) {
		innobase_parse_tbl_name(tbl_name, db_buf, tbl_buf, NULL);
		ut_ad(dd_mdl_verify(thd, db_buf, tbl_buf));
	}
#endif /* UNIV_DEBUG */

	const dd::Table*				dd_table;
	const dd::Partition*				dd_part	= nullptr;
	dd::cache::Dictionary_client*			dc
		= dd::get_dd_client(thd);
	dd::cache::Dictionary_client::Auto_releaser	releaser(dc);

	for (;;) {
		dd::String_type	schema;
		dd::String_type	tablename;
		if (dc->get_table_name_by_se_private_id(handler_name,
							table_id,
							&schema, &tablename)) {
			return(nullptr);
		}

		const bool	not_table = schema.empty();

		if (not_table) {
			if (dc->get_table_name_by_partition_se_private_id(
				    handler_name, table_id,
				    &schema, &tablename)
			    || schema.empty()) {
				return(nullptr);
			}
		}

		if (mdl != nullptr) {
			if (*mdl == reinterpret_cast<MDL_ticket*>(-1)) {
				*mdl = nullptr;
			}

			ut_ad((*mdl == nullptr) == (!tbl_name));
#ifdef UNIV_DEBUG
			if (*mdl != nullptr) {

				ut_ad(strcmp(schema.c_str(), db_buf) == 0);
				if (not_table) {
					ut_ad(strncmp(tablename.c_str(),
						      tbl_buf,
						      strlen(tablename.c_str()))
					      == 0);
				} else {
					ut_ad(strcmp(tablename.c_str(),
						     tbl_buf) == 0);
				}
			}
#endif

			if (*mdl == nullptr && dd_mdl_acquire(
				    thd, mdl,
				    schema.c_str(),
				    const_cast<char*>(tablename.c_str()))) {
				return(nullptr);
			}

			ut_ad(*mdl != nullptr);
		}

		if (dc->acquire(schema, tablename, &dd_table)
		    || dd_table == nullptr) {
			if (mdl != nullptr) {
				dd_mdl_release(thd, mdl);
			}
			return(nullptr);
		}

		const bool	is_part
			= (dd_table->partition_type() != dd::Table::PT_NONE);
		bool		same_name = not_table == is_part
			&& (not_table || dd_table->se_private_id() == table_id)
			&& dd_table->engine() == handler_name;

		if (same_name && is_part) {
			auto end = dd_table->partitions().end();
			auto i = std::search_n(
				dd_table->partitions().begin(), end, 1,
				table_id,
				[](const dd::Partition* p, table_id_t id)
				{
					return(p->se_private_id() == id);
				});

			if (i == end) {
				same_name = false;
			} else {
				dd_part = *i;
				ut_ad(dd_part_is_stored(dd_part));
			}
		}

		if (mdl != nullptr && !same_name) {
			dd_mdl_release(thd, mdl);
			continue;
		}

		ut_ad(same_name);
		break;
	}

	ut_ad(dd_part != nullptr
	      || dd_table->se_private_id() == table_id);
	ut_ad(dd_part == nullptr || dd_table == &dd_part->table());
	ut_ad(dd_part == nullptr || dd_part->se_private_id() == table_id);

	dict_table_t*	ib_table = nullptr;

	dd_table_open_on_dd_obj(
		dc, *dd_table, dd_part, tbl_name,
		ib_table, mdl == nullptr/*, table */, thd);

	if (mdl && ib_table == nullptr) {
		dd_mdl_release(thd, mdl);
	}

	return(ib_table);
}

/** Check if access to a table should be refused.
@param[in,out]	table	InnoDB table or partition
@return	error code
@retval	0	on success */
static MY_ATTRIBUTE((warn_unused_result))
int
dd_check_corrupted(dict_table_t*& table)
{

	if (table->is_corrupted()) {
		if (dict_table_is_sdi(table->id) || table->id <= 16) {
			my_error(ER_TABLE_CORRUPT, MYF(0),
				 "", table->name.m_name);
		} else {
			char	db_buf[NAME_LEN + 1];
			char	tbl_buf[NAME_LEN + 1];

			innobase_parse_tbl_name(
				table->name.m_name, db_buf, tbl_buf, NULL);
			my_error(ER_TABLE_CORRUPT, MYF(0),
				 db_buf, tbl_buf);
		}
		table = nullptr;
		return(HA_ERR_TABLE_CORRUPT);
	}

	dict_index_t* index = table->first_index();
	if (!dict_table_is_sdi(table->id)
	    && fil_space_get(index->space) == nullptr) {
		my_error(ER_TABLESPACE_MISSING, MYF(0), table->name.m_name);
		table = nullptr;
		return(HA_ERR_TABLESPACE_MISSING);
	}

	/* Ignore missing tablespaces for secondary indexes. */
	while ((index = index->next())) {
		if (!index->is_corrupted()
		    && fil_space_get(index->space) == nullptr) {
			dict_set_corrupted(index);
		}
	}

	return(0);
}

/** Open a persistent InnoDB table based on InnoDB table id, and
held Shared MDL lock on it.
@param[in]	table_id	table identifier
@param[in,out]	thd		current MySQL connection (for mdl)
@param[in,out]	mdl		metadata lock (*mdl set if table_id was found);
@param[in]	dict_lock	dict_sys mutex is held
mdl=NULL if we are resurrecting table IX locks in recovery
@return table
@retval NULL if the table does not exist or cannot be opened */
dict_table_t*
dd_table_open_on_id(
	table_id_t	table_id,
	THD*		thd,
	MDL_ticket**	mdl,
	bool		dict_locked)
{
	dict_table_t*   ib_table;
	const ulint     fold = ut_fold_ull(table_id);
	char		db_buf[NAME_LEN + 1];
	char		tbl_buf[NAME_LEN + 1];
	char		full_name[2 * (NAME_LEN + 1)];

	if (!dict_locked) {
		mutex_enter(&dict_sys->mutex);
	}

	HASH_SEARCH(id_hash, dict_sys->table_id_hash, fold,
		    dict_table_t*, ib_table, ut_ad(ib_table->cached),
		    ib_table->id == table_id);

	if (ib_table == NULL) {
		if (dict_table_is_sdi(table_id)) {
			/* The table is SDI table */
			space_id_t      space_id = dict_sdi_get_space_id(
				table_id);
			uint32_t        copy_num = dict_sdi_get_copy_num(
				table_id);

			/* Create in-memory table oject for SDI table */
			dict_index_t*   sdi_index = dict_sdi_create_idx_in_mem(
				space_id, copy_num, false, 0);

			if (sdi_index == NULL) {
				if (!dict_locked) {
					mutex_exit(&dict_sys->mutex);
				}
				return(NULL);
			}

			ib_table = sdi_index->table;

			ut_ad(ib_table != NULL);
			ib_table->acquire();
			mutex_exit(&dict_sys->mutex);
		} else {
			mutex_exit(&dict_sys->mutex);

			ib_table = dd_table_open_on_id_low(
				thd, mdl, nullptr, table_id);
		}
	} else if (mdl == nullptr || ib_table->is_temporary()
		   || dict_table_is_sdi(ib_table->id)) {
		if (dd_check_corrupted(ib_table)) {
			ut_ad(ib_table == nullptr);
		} else {
			ib_table->acquire();
		}
		mutex_exit(&dict_sys->mutex);
	} else {
		for (;;) {
			innobase_parse_tbl_name(
				ib_table->name.m_name, db_buf, tbl_buf, NULL);
			strcpy(full_name, ib_table->name.m_name);

			mutex_exit(&dict_sys->mutex);

			ut_ad(!ib_table->is_temporary());

			if (dd_mdl_acquire(thd, mdl, db_buf, tbl_buf)) {
				return(nullptr);
			}

			/* Re-lookup the table after acquiring MDL. */
			mutex_enter(&dict_sys->mutex);

			HASH_SEARCH(
				id_hash, dict_sys->table_id_hash, fold,
				dict_table_t*, ib_table,
				ut_ad(ib_table->cached),
				ib_table->id == table_id);


			if (ib_table != nullptr) {
				ulint	namelen = strlen(ib_table->name.m_name);

                                if (namelen != strlen(full_name)
				    || memcmp(ib_table->name.m_name,
					      full_name, namelen)) {
					dd_mdl_release(thd, mdl);
					continue;
				} else if (dd_check_corrupted(ib_table)) {
					ut_ad(ib_table == nullptr);
				} else {
					ib_table->acquire();
				}
			}

			mutex_exit(&dict_sys->mutex);
			break;
		}

		ut_ad(*mdl != nullptr);

		/* Now the table can't be found, release MDL,
		let dd_table_open_on_id_low() do the lock, as table
		name could be changed */
		if (ib_table == nullptr) {
			dd_mdl_release(thd, mdl);
			ib_table = dd_table_open_on_id_low(
				thd, mdl, nullptr, table_id);

			if (ib_table == nullptr && *mdl != nullptr) {
				dd_mdl_release(thd, mdl);
			}
		}
	}

	if (ib_table != nullptr) {
		if (table_id > 16 && !dict_table_is_sdi(table_id)
		    && !ib_table->ibd_file_missing
		    && !ib_table->is_fts_aux()) {
			if (!ib_table->stat_initialized) {
				dict_stats_init(ib_table);
			}
			ut_ad(ib_table->stat_initialized);
		}
		ut_ad(ib_table->n_ref_count > 0);
		MONITOR_INC(MONITOR_TABLE_REFERENCE);
	}

	if (dict_locked) {
		mutex_enter(&dict_sys->mutex);
	}
	return(ib_table);
}

/** Set the discard flag for a dd table.
@param[in,out]	thd	current thread
@param[in]	table	InnoDB table

@param[in]	discard	discard flag
@retval false if fail. */
bool
dd_table_discard_tablespace(
	THD*			thd,
	dict_table_t*		table,
	dd::Table*		table_def,
	bool			discard)
{
	bool			ret = false;

	DBUG_ENTER("dd_table_set_discard_flag");

	ut_ad(thd == current_thd);
#ifdef UNIV_DEBUG
	btrsea_sync_check       check(false);
	ut_ad(!sync_check_iterate(check));
#endif
	ut_ad(!srv_is_being_shutdown);

	if (table_def->se_private_id() != dd::INVALID_OBJECT_ID) {
		ut_ad(table_def->table().partitions()->empty());

		/* For discarding, we need to set new private
		id to dd_table */
		if (discard) {
			/* Set the new private id to dd_table object. */
			table_def->set_se_private_id(table->id);
		} else {
			ut_ad(table_def->se_private_id() == table->id);
		}

		/* Set index root page. */
		const dict_index_t* index = table->first_index();
		for (auto dd_index : *table_def->indexes()) {
			ut_ad(index != NULL);

			dd::Properties& p = dd_index->se_private_data();
			p.set_uint32(dd_index_key_strings[DD_INDEX_ROOT],
				index->page);
		}

		/* Set discard flag. */
		table_def->table().options().set_bool("discard",
							 discard);

		ret = true;
	} else {
		ret = false;
	}

	DBUG_RETURN(ret);
}

/** Open an internal handle to a persistent InnoDB table by name.
@param[in,out]	thd		current thread
@param[out]	mdl		metadata lock
@param[in]	name		InnoDB table name
@param[in]	dict_locked	has dict_sys mutex locked
@param[in]	ignore_err	whether to ignore err
@return handle to non-partitioned table
@retval NULL if the table does not exist */
dict_table_t*
dd_table_open_on_name(
	THD*			thd,
	MDL_ticket**		mdl,
	const char*		name,
	bool			dict_locked,
	ulint			ignore_err)
{
	DBUG_ENTER("dd_table_open_on_name");

#ifdef UNIV_DEBUG
	btrsea_sync_check       check(false);
	ut_ad(!sync_check_iterate(check));
#endif
	ut_ad(!srv_is_being_shutdown);

	char	db_buf[NAME_LEN + 1];
	char	tbl_buf[NAME_LEN + 1];

	bool	skip_mdl = !(thd && mdl);
	dict_table_t*	table = nullptr;

	/* Get pointer to a table object in InnoDB dictionary cache.
	For intrinsic table, get it from session private data */
	if (thd) {
		table = thd_to_innodb_session(
			thd)->lookup_table_handler(name);
	}

	if (table != nullptr) {
		table->acquire();
		DBUG_RETURN(table);
	}

	if (!innobase_parse_tbl_name(name, db_buf, tbl_buf, NULL)) {
		DBUG_RETURN(nullptr);
	}

	if (!skip_mdl && dd_mdl_acquire(thd, mdl, db_buf, tbl_buf)) {
		DBUG_RETURN(nullptr);
	}

	if (!dict_locked) {
		mutex_enter(&dict_sys->mutex);
	}

	table = dict_table_check_if_in_cache_low(name);

	if (table != nullptr) {
		table->acquire();
		if (!dict_locked) {
			mutex_exit(&dict_sys->mutex);
		}
		DBUG_RETURN(table);
	}

	mutex_exit(&dict_sys->mutex);

	const dd::Table*		dd_table = nullptr;
	dd::cache::Dictionary_client*	client = dd::get_dd_client(thd);
	dd::cache::Dictionary_client::Auto_releaser	releaser(client);

	if (client->acquire(db_buf, tbl_buf, &dd_table)
	    || dd_table == nullptr) {
		table = nullptr;
	} else {
		if (dd_table->se_private_id() == dd::INVALID_OBJECT_ID) {
			/* This must be a partitioned table. */
			ut_ad(!dd_table->partitions().empty());
			table = nullptr;
		} else {
			ut_ad(dd_table->partitions().empty());
			dd_table_open_on_dd_obj(
				client, *dd_table, nullptr, name,
				table, skip_mdl, thd);
		}
	}

	if (table && table->is_corrupted()
	    && !(ignore_err & DICT_ERR_IGNORE_CORRUPT)) {
		mutex_enter(&dict_sys->mutex);
		table->release();
		dict_table_remove_from_cache(table);
		table = NULL;
		mutex_exit(&dict_sys->mutex);
	}

	if (table == nullptr && mdl) {
		dd_mdl_release(thd, mdl);
		*mdl = nullptr;
	}

	if (dict_locked) {
		mutex_enter(&dict_sys->mutex);
	}

        DBUG_RETURN(table);
}

/** Close an internal InnoDB table handle.
@param[in,out]	table		InnoDB table handle
@param[in,out]	thd		current MySQL connection (for mdl)
@param[in,out]	mdl		metadata lock (will be set NULL)
@param[in]	dict_locked	whether we hold dict_sys mutex */
void
dd_table_close(
	dict_table_t*	table,
	THD*		thd,
	MDL_ticket**	mdl,
	bool		dict_locked)
{
	dict_table_close(table, dict_locked, false);

	const bool is_temp = table->is_temporary();

	MONITOR_DEC(MONITOR_TABLE_REFERENCE);

	if (!is_temp && mdl != nullptr
	    && (*mdl != reinterpret_cast<MDL_ticket*>(-1))) {
		dd_mdl_release(thd, mdl);
	}
}

/** Update filename of dd::Tablespace
@param[in]	dd_space_id	dd tablespace id
@param[in]	new_path	new data file path
@retval true if fail. */
bool
dd_tablespace_update_filename(
	dd::Object_id		dd_space_id,
	const char*		new_path)
{
	dd::Tablespace*		dd_space = nullptr;
	dd::Tablespace*		new_space = nullptr;
	bool			ret = false;
	THD*			thd = current_thd;

	DBUG_ENTER("dd_tablespace_update_for_rename");
#ifdef UNIV_DEBUG
	btrsea_sync_check       check(false);
	ut_ad(!sync_check_iterate(check));
#endif
	ut_ad(!srv_is_being_shutdown);
	ut_ad(new_path != NULL);

	dd::cache::Dictionary_client*	client = dd::get_dd_client(thd);
	dd::cache::Dictionary_client::Auto_releaser	releaser(client);

	/* Get the dd tablespace */

	if (client->acquire_uncached_uncommitted<dd::Tablespace>(
			dd_space_id, &dd_space)) {
		ut_a(false);
	}

	ut_a(dd_space != NULL);
	/* Acquire mdl share lock */
	if (dd::acquire_exclusive_tablespace_mdl(
		    thd, dd_space->name().c_str(), false)) {
		ut_a(false);
	}

	/* Acquire the new dd tablespace for modification */
	if (client->acquire_for_modification<dd::Tablespace>(
			dd_space_id, &new_space)) {
		ut_a(false);
	}

	ut_ad(new_space->files().size() == 1);
	dd::Tablespace_file*	dd_file = const_cast<
		dd::Tablespace_file*>(*(new_space->files().begin()));
	dd_file->set_filename(new_path);
	bool fail = client->update(new_space);
	ut_a(!fail);

	DBUG_RETURN(ret);
}

/** Validate the table format options.
@param[in]	zip_allowed	whether ROW_FORMAT=COMPRESSED is OK
@param[in]	strict		whether innodb_strict_mode=ON
@param[out]	is_redundant	whether ROW_FORMAT=REDUNDANT
@param[out]	blob_prefix	whether ROW_FORMAT=DYNAMIC
				or ROW_FORMAT=COMPRESSED
@param[out]	zip_ssize	log2(compressed page size),
				or 0 if not ROW_FORMAT=COMPRESSED
@retval true if invalid (my_error will have been called)
@retval false if valid */
bool
format_validate(
	THD*			m_thd,
        const TABLE*		m_form,
	bool			zip_allowed,
	bool			strict,
	bool*			is_redundant,
	bool*			blob_prefix,
	unsigned*		zip_ssize,
	bool			m_implicit)
{
	bool	is_temporary = false;
	ut_ad(m_thd != nullptr);
	ut_ad(!zip_allowed || srv_page_size <= UNIV_ZIP_SIZE_MAX);

	/* 1+log2(compressed_page_size), or 0 if not compressed */
	*zip_ssize			= 0;
	const unsigned	zip_ssize_max	= std::min(
		(ulint)UNIV_PAGE_SSIZE_MAX, (ulint)PAGE_ZIP_SSIZE_MAX);
	const char*	zip_refused	= zip_allowed
		? nullptr
		: srv_page_size <= UNIV_ZIP_SIZE_MAX
		? "innodb_file_per_table=OFF"
		: "innodb_page_size>16k";
	bool		invalid		= false;

	if (unsigned key_block_size = m_form->s->key_block_size) {
		unsigned	valid_zssize = 0;
		char		kbs[MY_INT32_NUM_DECIMAL_DIGITS
				    + sizeof "KEY_BLOCK_SIZE="];
		snprintf(kbs, sizeof kbs, "KEY_BLOCK_SIZE=%u",
			 key_block_size);
		for (unsigned kbsize = 1, zssize = 1;
		     zssize <= zip_ssize_max;
		     zssize++, kbsize <<= 1) {
			if (kbsize == key_block_size) {
				valid_zssize = zssize;
				break;
			}
		}

		if (valid_zssize == 0) {
			if (strict) {
				my_error(ER_WRONG_VALUE, MYF(0),
					 "KEY_BLOCK_SIZE",
					 kbs + sizeof "KEY_BLOCK_SIZE");
				invalid = true;
			} else {
				push_warning_printf(
					m_thd, Sql_condition::SL_WARNING,
					ER_WRONG_VALUE,
					ER_DEFAULT(ER_WRONG_VALUE),
					"KEY_BLOCK_SIZE",
					kbs + sizeof "KEY_BLOCK_SIZE");
			}
		} else if (!zip_allowed) {
			int		error = is_temporary
				? ER_UNSUPPORT_COMPRESSED_TEMPORARY_TABLE
				: ER_ILLEGAL_HA_CREATE_OPTION;

			if (strict) {
				my_error(error, MYF(0), innobase_hton_name,
					 kbs, zip_refused);
				invalid = true;
			} else {
				push_warning_printf(
					m_thd, Sql_condition::SL_WARNING,
					error,
					ER_DEFAULT(error),
					innobase_hton_name,
					kbs, zip_refused);
			}
		} else if (m_form->s->row_type == ROW_TYPE_DEFAULT
			   || m_form->s->row_type == ROW_TYPE_COMPRESSED) {
			ut_ad(m_form->s->real_row_type == ROW_TYPE_COMPRESSED);
			*zip_ssize = valid_zssize;
		} else {
			int	error = is_temporary
				? ER_UNSUPPORT_COMPRESSED_TEMPORARY_TABLE
				: ER_ILLEGAL_HA_CREATE_OPTION;
			const char* conflict = get_row_format_name(
				m_form->s->row_type);

			if (strict) {
				my_error(error, MYF(0),innobase_hton_name,
					 kbs, conflict);
				invalid = true;
			} else {
				push_warning_printf(
					m_thd, Sql_condition::SL_WARNING,
					error,
					ER_DEFAULT(error),
					innobase_hton_name, kbs, conflict);
			}
		}
	} else if (m_form->s->row_type != ROW_TYPE_COMPRESSED
		   || !is_temporary) {
		/* not ROW_FORMAT=COMPRESSED (nor KEY_BLOCK_SIZE),
		or not TEMPORARY TABLE */
	} else if (strict) {
		my_error(ER_UNSUPPORT_COMPRESSED_TEMPORARY_TABLE, MYF(0));
		invalid = true;
	} else {
		push_warning(m_thd, Sql_condition::SL_WARNING,
			     ER_UNSUPPORT_COMPRESSED_TEMPORARY_TABLE,
			     ER_THD(m_thd,
				    ER_UNSUPPORT_COMPRESSED_TEMPORARY_TABLE));
	}

	/* Check for a valid InnoDB ROW_FORMAT specifier and
	other incompatibilities. */
	rec_format_t	innodb_row_format = REC_FORMAT_DYNAMIC;

	switch (m_form->s->row_type) {
	case ROW_TYPE_DYNAMIC:
		ut_ad(*zip_ssize == 0);
		ut_ad(m_form->s->real_row_type == ROW_TYPE_DYNAMIC);
		break;
	case ROW_TYPE_COMPACT:
		ut_ad(*zip_ssize == 0);
		ut_ad(m_form->s->real_row_type == ROW_TYPE_COMPACT);
		innodb_row_format = REC_FORMAT_COMPACT;
		break;
	case ROW_TYPE_REDUNDANT:
		ut_ad(*zip_ssize == 0);
		ut_ad(m_form->s->real_row_type == ROW_TYPE_REDUNDANT);
		innodb_row_format = REC_FORMAT_REDUNDANT;
		break;
	case ROW_TYPE_FIXED:
	case ROW_TYPE_PAGED:
	case ROW_TYPE_NOT_USED:
		{
			const char* name = get_row_format_name(
				m_form->s->row_type);
			if (strict) {
				my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
					 innobase_hton_name, name);
				invalid = true;
			} else {
				push_warning_printf(
					m_thd, Sql_condition::SL_WARNING,
					ER_ILLEGAL_HA_CREATE_OPTION,
					ER_DEFAULT(ER_ILLEGAL_HA_CREATE_OPTION),
					innobase_hton_name, name);
			}
		}
		/* fall through */
	case ROW_TYPE_DEFAULT:
		switch (m_form->s->real_row_type) {
		case ROW_TYPE_FIXED:
		case ROW_TYPE_PAGED:
		case ROW_TYPE_NOT_USED:
		case ROW_TYPE_DEFAULT:
			/* get_real_row_type() should not return these */
			ut_ad(0);
			/* fall through */
		case ROW_TYPE_DYNAMIC:
			ut_ad(*zip_ssize == 0);
			break;
		case ROW_TYPE_COMPACT:
			ut_ad(*zip_ssize == 0);
			innodb_row_format = REC_FORMAT_COMPACT;
			break;
		case ROW_TYPE_REDUNDANT:
			ut_ad(*zip_ssize == 0);
			innodb_row_format = REC_FORMAT_REDUNDANT;
			break;
		case ROW_TYPE_COMPRESSED:
			innodb_row_format = REC_FORMAT_COMPRESSED;
			break;
		}

		if (*zip_ssize == 0) {
			/* No valid KEY_BLOCK_SIZE was specified,
			so do not imply ROW_FORMAT=COMPRESSED. */
			if (innodb_row_format == REC_FORMAT_COMPRESSED) {
				innodb_row_format = REC_FORMAT_DYNAMIC;
			}
			break;
		}
		/* fall through */
	case ROW_TYPE_COMPRESSED:
		if (is_temporary) {
			if (strict) {
				invalid = true;
			}
			/* ER_UNSUPPORT_COMPRESSED_TEMPORARY_TABLE
			was already reported. */
			ut_ad(m_form->s->real_row_type == ROW_TYPE_DYNAMIC);
			break;
		} else if (zip_allowed) {
			/* ROW_FORMAT=COMPRESSED without KEY_BLOCK_SIZE
			implies half the maximum compressed page size. */
			if (*zip_ssize == 0) {
				*zip_ssize = zip_ssize_max - 1;
			}
			ut_ad(m_form->s->real_row_type == ROW_TYPE_COMPRESSED);
			innodb_row_format = REC_FORMAT_COMPRESSED;
			break;
		}

		if (strict) {
			my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
				 innobase_hton_name,
				 "ROW_FORMAT=COMPRESSED", zip_refused);
			invalid = true;
		}
	}

	if (const char* algorithm = m_form->s->compress.length > 0
	    ? m_form->s->compress.str : nullptr) {
		Compression	compression;
		dberr_t		err = Compression::check(algorithm,
							 &compression);

		if (err == DB_UNSUPPORTED) {
			my_error(ER_WRONG_VALUE, MYF(0),
				 "COMPRESSION", algorithm);
			invalid = true;
		} else if (compression.m_type != Compression::NONE) {
			if (*zip_ssize != 0) {
				if (strict) {
					my_error(ER_ILLEGAL_HA_CREATE_OPTION,
						 MYF(0),
						 innobase_hton_name,
						 "COMPRESSION",
						 m_form->s->key_block_size
						 ? "KEY_BLOCK_SIZE"
						 : "ROW_FORMAT=COMPRESSED");
					invalid = true;
				} 
			}

			if (is_temporary) {
				my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
					 innobase_hton_name,
					 "COMPRESSION", "TEMPORARY");
				invalid = true;
			} else if (!m_implicit) {
				my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
					 innobase_hton_name,
					 "COMPRESSION", "TABLESPACE");
				invalid = true;
			}
		}
	}

	/* Check if there are any FTS indexes defined on this table. */
	for (uint i = 0; i < m_form->s->keys; i++) {
		const KEY*	key = &m_form->key_info[i];

		if (key->flags & HA_FULLTEXT) {
			/* We don't support FTS indexes in temporary
			tables. */
			if (is_temporary) {
				my_error(ER_INNODB_NO_FT_TEMP_TABLE, MYF(0));
				return(true);
			}
		}
	}

	ut_ad((*zip_ssize == 0)
	      == (innodb_row_format != REC_FORMAT_COMPRESSED));

	*is_redundant = false;
	*blob_prefix = false;

	switch (innodb_row_format) {
	case REC_FORMAT_REDUNDANT:
		*is_redundant = true;
		*blob_prefix = true;
		break;
	case REC_FORMAT_COMPACT:
		*blob_prefix = true;
		break;
	case REC_FORMAT_COMPRESSED:
		ut_ad(!is_temporary);
		break;
	case REC_FORMAT_DYNAMIC:
		break;
	}

	return(invalid);
}

/** Set the AUTO_INCREMENT attribute.
@param[in,out]	se_private_data		dd::Table::se_private_data
@param[in]	autoinc			the auto-increment value */
void
dd_set_autoinc(dd::Properties& se_private_data, uint64 autoinc)
{
	/* The value of "autoinc" here is the AUTO_INCREMENT attribute
	specified at table creation. AUTO_INCREMENT=0 will silently
	be treated as AUTO_INCREMENT=1. Likewise, if no AUTO_INCREMENT
	attribute was specified, the value would be 0. */

	if (autoinc > 0) {
		/* InnoDB persists the "previous" AUTO_INCREMENT value. */
		autoinc--;
	}

	uint64	version = 0;

	if (se_private_data.exists(dd_table_key_strings[DD_TABLE_AUTOINC])) {
		/* Increment the dynamic metadata version, so that
		any previously buffered persistent dynamic metadata
		will be ignored after this transaction commits. */

		if (!se_private_data.get_uint64(
			    dd_table_key_strings[DD_TABLE_VERSION],
			    &version)) {
			version++;
		} else {
			ut_ad(!"incomplete se_private_data");
		}
	}

	se_private_data.set_uint64(dd_table_key_strings[DD_TABLE_VERSION],
				   version);
	se_private_data.set_uint64(dd_table_key_strings[DD_TABLE_AUTOINC],
				   autoinc);
}

/** Create an index.
@param[in,out]	table		InnoDB table
@param[in]	strict		whether to be strict about the max record size
@param[in]	form		MySQL table structure
@param[in]	key_num		key_info[] offset
@return		error code
@retval		0 on success
@retval		HA_ERR_INDEX_COL_TOO_LONG if a column is too long
@retval		HA_ERR_TOO_BIG_ROW if the record is too long */
static MY_ATTRIBUTE((warn_unused_result))
int
dd_fill_one_dict_index(
	dict_table_t*		table,
	bool			strict,
	const TABLE_SHARE*	form,
	uint			key_num)
{
	const KEY&		key		= form->key_info[key_num];
	ulint			type = 0;
	unsigned		n_fields	= key.user_defined_key_parts;
	unsigned		n_uniq		= n_fields;
	std::bitset<REC_MAX_N_FIELDS>	indexed;

	/* This name cannot be used for a non-primary index */
	ut_ad(key_num == form->primary_key
	      || my_strcasecmp(system_charset_info,
			       key.name, primary_key_name) != 0);
	/* PARSER is only valid for FULLTEXT INDEX */
	ut_ad((key.flags & (HA_FULLTEXT | HA_USES_PARSER)) != HA_USES_PARSER);
	ut_ad(form->fields > 0);
	ut_ad(n_fields > 0);

	if (key.flags & HA_SPATIAL) {
		ut_ad(!table->is_intrinsic());
		type = DICT_SPATIAL;
		ut_ad(n_fields == 1);
	} else if (key.flags & HA_FULLTEXT) {
		ut_ad(!table->is_intrinsic());
		type = DICT_FTS;
		n_uniq = 0;
	} else if (key_num == form->primary_key) {
		ut_ad(key.flags & HA_NOSAME);
		ut_ad(n_uniq > 0);
		type = DICT_CLUSTERED | DICT_UNIQUE;
	} else {
		type = (key.flags & HA_NOSAME)
			? DICT_UNIQUE
			: 0;
	}

	ut_ad(!!(type & DICT_FTS) == (n_uniq == 0));

	dict_index_t*	index = dict_mem_index_create(
		table->name.m_name, key.name, 0, type, n_fields);

	index->n_uniq = n_uniq;

	const ulint	max_len	= DICT_MAX_FIELD_LEN_BY_FORMAT(table);
	DBUG_EXECUTE_IF("ib_create_table_fail_at_create_index",
			dict_mem_index_free(index);
			my_error(ER_INDEX_COLUMN_TOO_LONG, MYF(0), max_len);
			return(HA_ERR_TOO_BIG_ROW););

	for (unsigned i = 0; i < key.user_defined_key_parts; i++) {
		const KEY_PART_INFO*	key_part	= &key.key_part[i];
		unsigned		prefix_len	= 0;
		const Field*		field		= key_part->field;
		ut_ad(field == form->field[key_part->fieldnr - 1]);
		ut_ad(field == form->field[field->field_index]);

		if (field->is_virtual_gcol()) {
			index->type |= DICT_VIRTUAL;
		}

		bool	is_asc = true;

		if (key_part->key_part_flag & HA_REVERSE_SORT) {
			is_asc = false;
		}
			
		if (key.flags & HA_SPATIAL) {
			prefix_len = 0;
		} else if (key.flags & HA_FULLTEXT) {
			prefix_len = 0;
		} else if (key_part->key_part_flag & HA_PART_KEY_SEG) {
			/* SPATIAL and FULLTEXT index always are on
			full columns. */
			ut_ad(!(key.flags & (HA_SPATIAL | HA_FULLTEXT)));
			prefix_len = key_part->length;
			ut_ad(prefix_len > 0);
		} else {
			ut_ad(key.flags & (HA_SPATIAL | HA_FULLTEXT)
			      || (!is_blob(field->real_type())
				  && field->real_type()
				  != MYSQL_TYPE_GEOMETRY)
			      || key_part->length
			      >= (field->type() == MYSQL_TYPE_VARCHAR
				  ? field->key_length()
				  : field->pack_length()));
			prefix_len = 0;
		}

		if (key_part->length > max_len || prefix_len > max_len) {
			dict_mem_index_free(index);
			my_error(ER_INDEX_COLUMN_TOO_LONG, MYF(0), max_len);
			return(HA_ERR_INDEX_COL_TOO_LONG);
		}

		dict_col_t*	col = NULL;

		if (innobase_is_v_fld(field)) {
			dict_v_col_t*	v_col = dict_table_get_nth_v_col_mysql(
					table, field->field_index);
			col = reinterpret_cast<dict_col_t*>(v_col);
		} else {
			ulint	t_num_v = 0;
			for (ulint z = 0; z < field->field_index; z++) {
				if (innobase_is_v_fld(form->field[z])) {
					t_num_v++;
				}
			}

			col = &table->cols[field->field_index - t_num_v];
		}

		dict_index_add_col(index, table, col, prefix_len, is_asc);
	}

	ut_ad(((key.flags & HA_FULLTEXT) == HA_FULLTEXT)
	      == !!(index->type & DICT_FTS));

	index->n_user_defined_cols = key.user_defined_key_parts;

	int err = dict_index_add_to_cache(table, index, 0, FALSE);

	if (err != DB_SUCCESS) {
		ut_ad(0);
		return(HA_ERR_GENERIC);
	}

	index = UT_LIST_GET_LAST(table->indexes);

	if (index->type & DICT_FTS) {
		ut_ad((key.flags & HA_FULLTEXT) == HA_FULLTEXT);
		ut_ad(index->n_uniq == 0);
		ut_ad(n_uniq == 0);

		if (table->fts->cache == nullptr) {
			table->flags2 |= DICT_TF2_FTS;
			table->fts->cache = fts_cache_create(table);

			rw_lock_x_lock(&table->fts->cache->init_lock);
			/* Notify the FTS cache about this index. */
			fts_cache_index_cache_create(table, index);
			rw_lock_x_unlock(&table->fts->cache->init_lock);
		}
	}

	if (!strcmp(index->name, FTS_DOC_ID_INDEX_NAME)) {
		ut_ad(table->fts_doc_id_index == nullptr);
		table->fts_doc_id_index = index;
	}

	if (key.flags & HA_USES_PARSER) {
		ut_ad(index->type & DICT_FTS);
		index->parser = static_cast<st_mysql_ftparser*>(
			plugin_decl(key.parser)->info);
		index->is_ngram = strcmp(
			plugin_name(key.parser)->str,
			FTS_NGRAM_PARSER_NAME) == 0;
		DBUG_EXECUTE_IF(
			"fts_instrument_use_default_parser",
			index->parser = &fts_default_parser;);
	}

	return(0);
}

/** Parse MERGE_THRESHOLD value from a comment string.
@param[in]      thd     connection
@param[in]      str     string which might include 'MERGE_THRESHOLD='
@return value parsed
@retval dict_index_t::MERGE_THRESHOLD_DEFAULT for missing or invalid value. */
static
ulint
dd_parse_merge_threshold(THD* thd, const char* str)
{
        static constexpr char   label[] = "MERGE_THRESHOLD=";

        if (const char* pos = strstr(str, label)) {
                pos += (sizeof label) - 1;

                int     ret = atoi(pos);

                if (ret > 0
                    && unsigned(ret) <= DICT_INDEX_MERGE_THRESHOLD_DEFAULT) {
                        return(static_cast<ulint>(ret));
                }

                push_warning_printf(
                        thd, Sql_condition::SL_WARNING,
                        WARN_OPTION_IGNORED,
                        ER_DEFAULT(WARN_OPTION_IGNORED),
                        "MERGE_THRESHOLD");
        }

        return(DICT_INDEX_MERGE_THRESHOLD_DEFAULT);
}

/** Copy attributes from MySQL TABLE_SHARE into an InnoDB table object.
@param[in,out]	thd		thread context
@param[in,out]	table		InnoDB table
@param[in]	table_share	TABLE_SHARE */
inline
void
dd_copy_from_table_share(
	THD*			thd,
	dict_table_t*		table,
	const TABLE_SHARE*	table_share)
{
	if (table->is_temporary()) {
		//table->set_persistent_stats(false);
		dict_stats_set_persistent(table, false, true);
	} else {
		switch (table_share->db_create_options
			& (HA_OPTION_STATS_PERSISTENT
			   | HA_OPTION_NO_STATS_PERSISTENT)) {
		default:
			/* If a CREATE or ALTER statement contains
			STATS_PERSISTENT=0 STATS_PERSISTENT=1,
			it will be interpreted as STATS_PERSISTENT=1. */
		case HA_OPTION_STATS_PERSISTENT:
			//table->set_persistent_stats(true);
			dict_stats_set_persistent(table, true, false);
			break;
		case HA_OPTION_NO_STATS_PERSISTENT:
			//table->set_persistent_stats(false);
			dict_stats_set_persistent(table, false, true);
			break;
		case 0:
			break;
		}
	}

	dict_stats_auto_recalc_set(
		table,
		table_share->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON,
		table_share->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF);


	table->stats_sample_pages = table_share->stats_sample_pages;

	const ulint	merge_threshold_table = table_share->comment.str
		? dd_parse_merge_threshold(thd, table_share->comment.str)
		: DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
	dict_index_t*	index	= table->first_index();

	index->merge_threshold = merge_threshold_table;

	if (dict_index_is_auto_gen_clust(index)) {
		index = index->next();
	}

	for (uint i = 0; i < table_share->keys; i++) {
		const KEY*	key_info = &table_share->key_info[i];

		ut_ad(index != nullptr);

		if (key_info->flags & HA_USES_COMMENT
		    && key_info->comment.str != nullptr) {
			index->merge_threshold = dd_parse_merge_threshold(
				thd, key_info->comment.str);
		} else {
			index->merge_threshold = merge_threshold_table;
		}

		index = index->next();

		/* Skip hidden FTS_DOC_ID index */
		if (index != nullptr && index->hidden) {
			ut_ad(index != nullptr);
			ut_ad(strcmp(index->name, FTS_DOC_ID_INDEX_NAME) == 0);
			index = index->next();
		}
	}

	ut_ad(index == nullptr);
}

/** Instantiate index related metadata
@param[in,out]	dd_table	Global DD table metadata
@param[in]	m_form		MySQL table definition
@param[in,out]	m_table		InnoDB table definition
@param[in]	m_create_info	create table information
@param[in]	zip_allowed	if compression is allowed
@param[in]	strict		if report error in strict mode
@param[in]	m_thd		THD instance
@param[in]	m_skip_mdl	whether to skip MDL lock
@return 0 if successful, otherwise error number */
inline
int
dd_fill_dict_index(
	const dd::Table&	dd_table,
        const TABLE*		m_form,
	dict_table_t*		m_table,
	HA_CREATE_INFO*		m_create_info,
	bool			zip_allowed,
	bool			strict,
	THD*			m_thd,
        bool                    m_skip_mdl)
{
	int		error = 0;

	/* Create the keys */
	if (m_form->s->keys == 0 || m_form->s->primary_key == MAX_KEY) {
		/* Create an index which is used as the clustered index;
		order the rows by the hidden InnoDB column DB_ROW_ID. */
		dict_index_t*	index = dict_mem_index_create(
			m_table->name.m_name, "GEN_CLUST_INDEX",
			0, DICT_CLUSTERED, 0);
		index->n_uniq = 0;

		dberr_t	new_err = dict_index_add_to_cache(
			m_table, index, index->page, FALSE);
		if (new_err != DB_SUCCESS) {
			error = HA_ERR_GENERIC;
			goto dd_error;
		}
	} else {
		/* In InnoDB, the clustered index must always be
		created first. */
		error = dd_fill_one_dict_index(
			m_table, strict, m_form->s, m_form->s->primary_key);
		if (error != 0) {
			goto dd_error;
		}
	}

	for (uint i = !m_form->s->primary_key; i < m_form->s->keys; i++) {
		error = dd_fill_one_dict_index(
			m_table, strict, m_form->s, i);
		if (error != 0) {
			goto dd_error;
		}
	}

	if (dict_table_has_fts_index(m_table)) {
		ut_ad(DICT_TF2_FLAG_IS_SET(m_table, DICT_TF2_FTS));
	}

	/* Create the ancillary tables that are common to all FTS indexes on
	this table. */
	if (DICT_TF2_FLAG_IS_SET(m_table, DICT_TF2_FTS_HAS_DOC_ID)
	    || DICT_TF2_FLAG_IS_SET(m_table, DICT_TF2_FTS)) {
		fts_doc_id_index_enum	ret;

		ut_ad(!m_table->is_intrinsic());
		/* Check whether there already exists FTS_DOC_ID_INDEX */
		ret = innobase_fts_check_doc_id_index_in_def(
			m_form->s->keys, m_form->key_info);

		switch (ret) {
		case FTS_INCORRECT_DOC_ID_INDEX:
			push_warning_printf(m_thd,
					    Sql_condition::SL_WARNING,
					    ER_WRONG_NAME_FOR_INDEX,
					    " InnoDB: Index name %s is reserved"
					    " for the unique index on"
					    " FTS_DOC_ID column for FTS"
					    " Document ID indexing"
					    " on table %s. Please check"
					    " the index definition to"
					    " make sure it is of correct"
					    " type\n",
					    FTS_DOC_ID_INDEX_NAME,
					    m_table->name.m_name);

			if (m_table->fts) {
				fts_free(m_table);
			}

			my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
				 FTS_DOC_ID_INDEX_NAME);
			error = -1;
			return(error);
		case FTS_EXIST_DOC_ID_INDEX:
			break;
		case FTS_NOT_EXIST_DOC_ID_INDEX:
			dict_index_t*	doc_id_index;
			doc_id_index = dict_mem_index_create(
				m_table->name.m_name,
				FTS_DOC_ID_INDEX_NAME,
				0, DICT_UNIQUE, 1);
			doc_id_index->add_field(FTS_DOC_ID_COL_NAME, 0, true);

			dberr_t	new_err = dict_index_add_to_cache(
				m_table, doc_id_index,
				doc_id_index->page, FALSE);
			if (new_err != DB_SUCCESS) {
				error = HA_ERR_GENERIC;
				goto dd_error;
			}

			doc_id_index = UT_LIST_GET_LAST(m_table->indexes);
			doc_id_index->hidden = true;

			/* Adjust index order */
			innobase_adjust_fts_doc_id_index_order(
				dd_table, m_table);
		}

		/* Cache all the FTS indexes on this table in the FTS
		specific structure. They are used for FTS indexed
		column update handling. */
		if (dict_table_has_fts_index(m_table)) {

			fts_t*	fts = m_table->fts;
			ut_a(fts != nullptr);

			dict_table_get_all_fts_indexes(m_table, m_table->fts->indexes);
		}

		ulint	fts_doc_id_col = ULINT_UNDEFINED;

		ret = innobase_fts_check_doc_id_index(
			m_table, nullptr, &fts_doc_id_col);

		if (ret != FTS_INCORRECT_DOC_ID_INDEX) {
			ut_ad(m_table->fts->doc_col == ULINT_UNDEFINED);
			m_table->fts->doc_col = fts_doc_id_col;
			ut_ad(m_table->fts->doc_col != ULINT_UNDEFINED);

			m_table->fts_doc_id_index =
				dict_table_get_index_on_name(
					m_table, FTS_DOC_ID_INDEX_NAME);
		}
	}

	if (Field** autoinc_col = m_form->s->found_next_number_field) {
		const dd::Properties& p = dd_table.se_private_data();
		dict_table_autoinc_set_col_pos(
			m_table, (*autoinc_col)->field_index);
		uint64	version, autoinc = 0;
		if (p.get_uint64(dd_table_key_strings[DD_TABLE_VERSION],
				 &version)
		    || p.get_uint64(dd_table_key_strings[DD_TABLE_AUTOINC],
				    &autoinc)) {
			ut_ad(!"problem setting AUTO_INCREMENT");
			error = HA_ERR_CRASHED;
			goto dd_error;
		}

		dict_table_autoinc_lock(m_table);
		dict_table_autoinc_initialize(m_table, autoinc + 1);
		dict_table_autoinc_unlock(m_table);
		m_table->autoinc_persisted = autoinc;
	}

	if (error == 0) {
		dd_copy_from_table_share(m_thd, m_table, m_form->s);
		ut_ad(!m_table->is_temporary()
		      || !dict_table_page_size(m_table).is_compressed());
		if (!m_table->is_temporary()) {
			//m_table->stats_lock_create();
			dict_table_stats_latch_create(m_table, true);
		}
	} else {
dd_error:
		dict_mem_table_free(m_table);
	}

	return(error);
}

/** Determine if a table contains a fulltext index.
@param[in]      table		dd::Table
@return whether the table contains any fulltext index */
inline
bool
dd_table_contains_fulltext(const dd::Table& table)
{
        for (const dd::Index* index : table.indexes()) {
                if (index->type() == dd::Index::IT_FULLTEXT) {
                        return(true);
                }
        }
        return(false);
}

inline std::nullptr_t dd_part_name(const dd::Table*) {return nullptr;}
inline std::nullptr_t dd_subpart_name(const dd::Table*) {return nullptr;}

/** Get the parent partition of a partition
@param[in]      part    partition
@return parent partition
@retval nullptr if not subpartitioned */
inline const dd::Partition* dd_parent(const dd::Partition& part)
{
        return(part.parent());
}

/** Get the partition name.
@param[in]      part    partition or subpartition
@return partition name */
inline
const char*
dd_part_name(const dd::Partition* part)
{
        if (const dd::Partition* parent = dd_parent(*part)) {
                ut_ad(part->level() == 1);
                part = parent;
        }

        ut_ad(part->level() == 0);
        return(part->name().c_str());
}

/** Get the subpartition name.
@param[in]      part    partition or subpartition
@return subpartition name
@retval nullptr if not subpartitioned */
inline
const char*
dd_subpart_name(const dd::Partition* part)
{
        return(part->parent() ? part->name().c_str() : nullptr);
}

/** Instantiate in-memory InnoDB table metadata (dict_table_t),
without any indexes.
@tparam		Table		dd::Table or dd::Partition
@param[in]	dd_part		Global Data Dictionary metadata,
				or NULL for internal temporary table
@param[in]	norm_name	normalized table name
@param[in]	zip_allowed	whether ROW_FORMAT=COMPRESSED is OK
@param[in]	strict		whether to use innodb_strict_mode=ON
@return ER_ level error
@retval 0 on success */
template<typename Table>
inline
dict_table_t*
dd_fill_dict_table(
	const Table*		dd_part,
        const TABLE*            m_form,
	const char*		norm_name,
	HA_CREATE_INFO*		m_create_info,
	bool			zip_allowed,
	bool			strict,
	THD*			m_thd,
        bool                    m_skip_mdl,
	bool			m_implicit)
{
	mem_heap_t*	heap;
	bool		is_encrypted = false;
	bool		is_discard = false;

	ut_ad(dd_part != NULL);
	ut_ad(m_thd != nullptr);
	ut_ad(norm_name != nullptr);
	ut_ad(m_create_info == nullptr
	      || m_form->s->row_type == m_create_info->row_type);
	ut_ad(m_create_info == nullptr
	      || m_form->s->key_block_size == m_create_info->key_block_size);
	ut_ad(dd_part != nullptr);

	if (m_form->s->fields > REC_MAX_N_USER_FIELDS) {
		my_error(ER_TOO_MANY_FIELDS, MYF(0));
		return(NULL);
	}

	/* Set encryption option. */
	dd::String_type	encrypt;
	dd_part->table().options().get("encrypt_type", encrypt);

	if (!Encryption::is_none(encrypt.c_str())) {
		ut_ad(innobase_strcasecmp(encrypt.c_str(), "y") == 0);
		is_encrypted = true;
	}

	/* Check discard flag. */
	if (dd_part->table().options().exists("discard")) {
		dd_part->table().options().get_bool("discard", &is_discard);
	}

	const unsigned	n_mysql_cols = m_form->s->fields;

	bool	has_doc_id = false;

	/* First check if dd::Table contains the right hidden column
	as FTS_DOC_ID */
	const dd::Column*	doc_col = dd_find_column(
		const_cast<dd::Table*>(&dd_part->table()), FTS_DOC_ID_COL_NAME);

	/* Check weather there is a proper typed FTS_DOC_ID */
	if (doc_col && doc_col->type() == dd::enum_column_types::LONGLONG
	    && !doc_col->is_nullable()) {
		has_doc_id = true;
	}

	const bool	fulltext = dd_part != nullptr
		&& dd_table_contains_fulltext(dd_part->table());

	if (fulltext) {
		ut_ad(has_doc_id);
	}

	bool	add_doc_id = false;

	/* Need to add FTS_DOC_ID column if it is not defined by user,
	since TABLE_SHARE::fields does not contain it if it is a hidden col */
	if (has_doc_id && doc_col->is_hidden()) {
#ifdef UNIV_DEBUG
		ulint	doc_id_col;
		ut_ad(!create_table_check_doc_id_col(
			m_thd, m_form, &doc_id_col));
#endif
		add_doc_id = true;
	}

	const unsigned	n_cols = n_mysql_cols + add_doc_id;

	bool		is_redundant;
	bool		blob_prefix;
	unsigned	zip_ssize;

	if (format_validate(m_thd, m_form, zip_allowed, strict,
			    &is_redundant, &blob_prefix, &zip_ssize,
			    m_implicit)) {
		return(NULL);
	}

	ulint	n_v_cols = 0;

	/* Find out the number of virtual columns */
	for (ulint i = 0; i < m_form->s->fields; i++) {
                Field*  field = m_form->field[i];

		if (innobase_is_v_fld(field)) {
			n_v_cols++;
		}
	}

	ut_ad(n_v_cols <= n_cols);

	dict_table_t*	m_table = dict_mem_table_create(
		norm_name, 0, n_cols, n_v_cols, 0, 0);

	m_table->id = dd_part->se_private_id();

	if (dd_part->se_private_data().exists(
		dd_table_key_strings[DD_TABLE_DATA_DIRECTORY])) {
		m_table->flags |= DICT_TF_MASK_DATA_DIR;
	}

	fts_aux_table_t aux_table;
	if (fts_is_aux_table_name(&aux_table, norm_name, strlen(norm_name))) {
		DICT_TF2_FLAG_SET(m_table, DICT_TF2_AUX);
	}

	if (is_encrypted) {
		DICT_TF2_FLAG_SET(m_table, DICT_TF2_ENCRYPTION);
	}

	if (is_discard) {
		m_table->ibd_file_missing = true;
		m_table->flags2 |= DICT_TF2_DISCARDED;
	}

	if (!is_redundant) {
		m_table->flags |= DICT_TF_COMPACT;
	}

	if (m_implicit) {
		m_table->flags2 |= DICT_TF2_USE_FILE_PER_TABLE;
	} else {
		m_table->flags |= (1 << DICT_TF_POS_SHARED_SPACE);
	}

	if (!blob_prefix) {
		m_table->flags |= (1 << DICT_TF_POS_ATOMIC_BLOBS);
	}

	if (zip_ssize != 0) {
		m_table->flags |= (zip_ssize << DICT_TF_POS_ZIP_SSIZE);
	}

	if (has_doc_id) {
		m_table->fts = NULL;
		if (fulltext) {
			DICT_TF2_FLAG_SET(m_table, DICT_TF2_FTS);
		}

		if (add_doc_id) {
			DICT_TF2_FLAG_SET(m_table, DICT_TF2_FTS_HAS_DOC_ID);
		}

		if (fulltext || add_doc_id) {
			m_table->fts = fts_create(m_table);
			m_table->fts->cache = fts_cache_create(m_table);
		}
	} else {
		m_table->fts = NULL;
	}

	bool	is_temp = !dd_part->is_persistent()
		&& (dd_part->se_private_id()
		    >= dict_sys_t::NUM_HARD_CODED_TABLES);
	if (is_temp) {
		m_table->flags2 |= DICT_TF2_TEMPORARY;
	}

	m_table->flags2 |= DICT_TF2_FTS_AUX_HEX_NAME;

	heap = mem_heap_create(1000);

	for (unsigned i = 0; i < n_mysql_cols; i++) {
		const Field*	field = m_form->field[i];
		unsigned	mtype;
		unsigned	prtype = 0;
		unsigned	col_len = field->pack_length();

		/* The MySQL type code has to fit in 8 bits
		in the metadata stored in the InnoDB change buffer. */
		ut_ad(field->charset() == nullptr
		      || field->charset()->number <= MAX_CHAR_COLL_NUM);
		ut_ad(field->charset() == nullptr
		      || field->charset()->number > 0);

		ulint	nulls_allowed;
		ulint	unsigned_type;
		ulint	binary_type;
		ulint	long_true_varchar;
		ulint	charset_no;
		mtype = get_innobase_type_from_mysql_type(
			&unsigned_type, field);

		nulls_allowed = field->real_maybe_null() ? 0 : DATA_NOT_NULL;
		binary_type = field->binary() ? DATA_BINARY_TYPE : 0;

		charset_no = 0;
		if (dtype_is_string_type(mtype)) {
			charset_no = static_cast<ulint>(
				field->charset()->number);
		}

		long_true_varchar = 0;
		if (field->type() == MYSQL_TYPE_VARCHAR) {
			col_len -= ((Field_varstring*) field)->length_bytes;

			if (((Field_varstring*) field)->length_bytes == 2) {
				long_true_varchar = DATA_LONG_TRUE_VARCHAR;
			}
		}

		ulint	is_virtual = (innobase_is_v_fld(field))
					? DATA_VIRTUAL : 0;

		bool    is_stored = innobase_is_s_fld(field);

		if (!is_virtual) {
			prtype = dtype_form_prtype(
				(ulint) field->type() | nulls_allowed
				| unsigned_type | binary_type
				| long_true_varchar, charset_no);
			dict_mem_table_add_col(m_table, heap, field->field_name,
					       mtype, prtype, col_len);
		} else {
			prtype = dtype_form_prtype(
				(ulint) field->type() | nulls_allowed
				| unsigned_type | binary_type
				| long_true_varchar | is_virtual, charset_no);
			dict_mem_table_add_v_col(
				m_table, heap, field->field_name, mtype,
				prtype, col_len, i,
				field->gcol_info->non_virtual_base_columns());
		}

		if (is_stored) {
			ut_ad(!is_virtual);
			/* Added stored column in m_s_cols list. */
			dict_mem_table_add_s_col(
				m_table,
				field->gcol_info->non_virtual_base_columns());
		}
	}

	ulint	j = 0;

	if (m_table->n_v_cols > 0) {
		for (unsigned i = 0; i < n_mysql_cols; i++) {
			dict_v_col_t*	v_col;

			Field*  field = m_form->field[i];

			if (!innobase_is_v_fld(field)) {
				continue;
			}

			v_col = dict_table_get_nth_v_col(m_table, j);

			j++;

			innodb_base_col_setup(m_table, field, v_col);
		}
	}

	if (add_doc_id) {
		/* Add the hidden FTS_DOC_ID column. */
		fts_add_doc_id_column(m_table, heap);
	}

	/* Add system columns to make adding index work */
	dict_table_add_system_columns(m_table, heap);

	mem_heap_free(heap);

	return(m_table);
}

/** Check if a tablespace is implicit.
@param[in]      dd_space        tablespace metadata
@param[in]      space_id        InnoDB tablespace ID
@retval true    if the tablespace is implicit (file per table or partition)
@retval false   if the tablespace is shared (predefined or user-created) */
bool
dd_tablespace_is_implicit(const dd::Tablespace* dd_space, space_id_t space_id)
{
        const char*     name = dd_space->name().c_str();
        const char*     suffix = &name[sizeof reserved_implicit_name];
        char*           end;

        ut_d(uint32 id);
        ut_ad(!dd_space->se_private_data().get_uint32(
                      dd_space_key_strings[DD_SPACE_ID], &id));
        ut_ad(id == space_id);

	/* Check if the name starts with "innodb_file_per_table" */
        if (strncmp(name, dict_sys_t::file_per_table_name, suffix - name - 1)) {
                /* Not starting with innodb_file_per_table. */
                return(false);
        }

        if (suffix[-1] != '.' || suffix[0] == '\0'
            || strtoul(suffix, &end, 10) != space_id
            || *end != '\0') {
                ut_ad(!"invalid implicit tablespace name");
                return(false);
        }

        return(true);
}

/** Determine if a tablespace is implicit.
@param[in,out]  client          data dictionary client
@param[in]      dd_space_id     dd tablespace id
@param[out]     implicit        whether the tablespace is implicit tablespace
@retval false   on success
@retval true    on failure */
bool
dd_tablespace_is_implicit(
        dd::cache::Dictionary_client*   client,
	dd::Object_id			dd_space_id,
        bool*                           implicit)
{
        dd::Tablespace*		dd_space = NULL;
        uint32			id = 0;

        const bool              fail
		= client->acquire_uncached_uncommitted<dd::Tablespace>(
			dd_space_id, &dd_space)
                || dd_space == nullptr
                || dd_space->se_private_data().get_uint32(
                        dd_space_key_strings[DD_SPACE_ID], &id);

        if (!fail) {
                *implicit = dd_tablespace_is_implicit(dd_space, id);
        }

        return(fail);
}

/** Load foreign key constraint info for the dd::Table object.
@param[out]	m_table		InnoDB table handle
@param[in]	dd_table	Global DD table
@param[in]	col_names	column names, or NULL
@param[in]	dict_locked	True if dict_sys->mutex is already held,
				otherwise false
@return DB_SUCCESS 	if successfully load FK constraint */
dberr_t
dd_table_load_fk_from_dd(
	dict_table_t*			m_table,
	const dd::Table*		dd_table,
	const char**			col_names,
	bool				dict_locked)
{
	dberr_t	err = DB_SUCCESS;

	/* Now fill in the foreign key info */
	for (const dd::Foreign_key* key : dd_table->foreign_keys()) {
		char	buf[2 * NAME_CHAR_LEN * 5 + 2 + 1];

		dd::String_type	db_name = key->referenced_table_schema_name();
		dd::String_type	tb_name = key->referenced_table_name();

		bool	truncated;
		build_table_filename(buf, sizeof(buf),
				     db_name.c_str(), tb_name.c_str(),
				     NULL, 0, &truncated);
		ut_ad(!truncated);
		char            norm_name[FN_REFLEN];
		normalize_table_name(norm_name, buf);

		dict_foreign_t* foreign = dict_mem_foreign_create();
		foreign->foreign_table_name = mem_heap_strdup(
			foreign->heap, m_table->name.m_name);

		dict_mem_foreign_table_name_lookup_set(foreign, TRUE);

		foreign->referenced_table_name = mem_heap_strdup(
			foreign->heap, norm_name);
		dict_mem_referenced_table_name_lookup_set(foreign, TRUE);
		ulint	db_len = dict_get_db_name_len(m_table->name.m_name);

		ut_ad(db_len > 0);

		memcpy(buf, m_table->name.m_name, db_len);

		buf[db_len] = '\0';

		snprintf(norm_name, sizeof norm_name, "%s/%s",
			 buf, key->name().c_str());

		foreign->id = mem_heap_strdup(
			foreign->heap, norm_name);

		switch (key->update_rule()) {
		case dd::Foreign_key::RULE_NO_ACTION:
			foreign->type = DICT_FOREIGN_ON_UPDATE_NO_ACTION;
			break;
		case dd::Foreign_key::RULE_RESTRICT:
		case dd::Foreign_key::RULE_SET_DEFAULT:
			foreign->type = 0;
			break;
		case dd::Foreign_key::RULE_CASCADE:
			foreign->type = DICT_FOREIGN_ON_UPDATE_CASCADE;
			break;
		case dd::Foreign_key::RULE_SET_NULL:
			foreign->type = DICT_FOREIGN_ON_UPDATE_SET_NULL;
			break;
		default:
			ut_ad(0);
		}

		switch (key->delete_rule()) {
		case dd::Foreign_key::RULE_NO_ACTION:
			foreign->type |= DICT_FOREIGN_ON_DELETE_NO_ACTION;
		case dd::Foreign_key::RULE_RESTRICT:
		case dd::Foreign_key::RULE_SET_DEFAULT:
			break;
		case dd::Foreign_key::RULE_CASCADE:
			foreign->type |= DICT_FOREIGN_ON_DELETE_CASCADE;
			break;
		case dd::Foreign_key::RULE_SET_NULL:
			foreign->type |= DICT_FOREIGN_ON_DELETE_SET_NULL;
			break;
		default:
			ut_ad(0);
		}

		foreign->n_fields = key->elements().size();

		foreign->foreign_col_names = static_cast<const char**>(
			mem_heap_alloc(foreign->heap,
				       foreign->n_fields * sizeof(void*)));

		foreign->referenced_col_names = static_cast<const char**>(
			mem_heap_alloc(foreign->heap,
				       foreign->n_fields * sizeof(void*)));

		ulint	num_ref = 0;

		for (const dd::Foreign_key_element* key_e : key->elements()) {
			dd::String_type	ref_col_name
				 = key_e->referenced_column_name();

			foreign->referenced_col_names[num_ref]
				= mem_heap_strdup(foreign->heap,
						  ref_col_name.c_str());
			ut_ad(ref_col_name.c_str());

			const dd::Column*	f_col =  &key_e->column();
			foreign->foreign_col_names[num_ref]
				= mem_heap_strdup(
					foreign->heap, f_col->name().c_str());
			num_ref++;
		}

		if (!dict_locked) {
			mutex_enter(&dict_sys->mutex);
		}
#ifdef UNIV_DEBUG
		dict_table_t*   for_table;

		for_table = dict_table_check_if_in_cache_low(
			foreign->foreign_table_name_lookup);

		ut_ad(for_table);
#endif

		/* Fill in foreign->foreign_table and index, then add to
		dict_table_t */
		err = dict_foreign_add_to_cache(
			foreign, col_names, FALSE, DICT_ERR_IGNORE_NONE);
		ut_ad(err == DB_SUCCESS);
		if (!dict_locked) {
			mutex_exit(&dict_sys->mutex);
		}

		/* Set up the FK virtual column info */
		dict_mem_table_free_foreign_vcol_set(m_table);
		dict_mem_table_fill_foreign_vcol_set(m_table);
	}
	return(err);
}

/** Load foreign key constraint for the table. Note, it could also open
the foreign table, if this table is referenced by the foreign table
@param[in,out]	client		data dictionary client
@param[in]	tbl_name	Table Name
@param[in]	col_names	column names, or NULL
@param[out]	m_table		InnoDB table handle
@param[in]	dd_table	Global DD table
@param[in]	thd		thread THD
@param[in]	dict_locked	True if dict_sys->mutex is already held,
				otherwise false
@param[in]	char_charsets	whether to check charset compatibility
@param[in,out]	fk_tables	name list for tables that refer to this table
@return DB_SUCCESS 	if successfully load FK constraint */
dberr_t
dd_table_load_fk(
	dd::cache::Dictionary_client*	client,
	const char*			tbl_name,
	const char**			col_names,
	dict_table_t*			m_table,
	const dd::Table*		dd_table,
	THD*				thd,
	bool				dict_locked,
	bool				check_charsets,
	dict_names_t*			fk_tables)
{
	dberr_t	err = DB_SUCCESS;

	err = dd_table_load_fk_from_dd(m_table, dd_table, col_names,
				       dict_locked);

	if (err != DB_SUCCESS) {
		return(err);
	}

	if (dict_locked) {
		mutex_exit(&dict_sys->mutex);
	}

	err = dd_table_check_for_child(client, tbl_name, col_names, m_table,
				       dd_table, thd, check_charsets,
				       fk_tables);

	if (dict_locked) {
		mutex_enter(&dict_sys->mutex);
	}

	return(err);
}

/** Load foreign key constraint for the table. Note, it could also open
the foreign table, if this table is referenced by the foreign table
@param[in,out]	client		data dictionary client
@param[in]	tbl_name	Table Name
@param[in]	col_names	column names, or NULL
@param[out]	m_table		InnoDB table handle
@param[in]	dd_table	Global DD table
@param[in]	thd		thread THD
@param[in]	char_charsets	whether to check charset compatibility
@param[in,out]	fk_tables	name list for tables that refer to this table
@return DB_SUCCESS 	if successfully load FK constraint */
dberr_t
dd_table_check_for_child(
	dd::cache::Dictionary_client*	client,
	const char*			tbl_name,
	const char**			col_names,
	dict_table_t*			m_table,
	const dd::Table*		dd_table,
	THD*				thd,
	bool				check_charsets,
	dict_names_t*			fk_tables)
{
	dberr_t	err = DB_SUCCESS;

	/* TODO: NewDD: Temporary ignore system table until WL#6049 inplace */
	if (!strstr(tbl_name, "mysql") && fk_tables != nullptr) {
		std::vector<dd::String_type>	child_schema;
		std::vector<dd::String_type>	child_name;

		char    name_buf1[NAME_LEN + 1];
                char    name_buf2[NAME_LEN + 1];

		innobase_parse_tbl_name(m_table->name.m_name,
					name_buf1, name_buf2, NULL);

		client->fetch_fk_children_uncached(name_buf1, name_buf2,
						   &child_schema, &child_name);
		std::vector<dd::String_type>::iterator it = child_name.begin();
		for (auto& db_name : child_schema) {
			dd::String_type	tb_name = *it;
			char	buf[2 * NAME_CHAR_LEN * 5 + 2 + 1];
			bool	truncated;
			build_table_filename(
				buf, sizeof(buf), db_name.c_str(),
				tb_name.c_str(), NULL, 0, &truncated);
			ut_ad(!truncated);
			char            full_name[FN_REFLEN];
			normalize_table_name(full_name, buf);

			mutex_enter(&dict_sys->mutex);

			/* Load the foreign table first */
			dict_table_t*	foreign_table =
				dd_table_open_on_name_in_mem(
					full_name, true,
					DICT_ERR_IGNORE_NONE);

			if (foreign_table) {
				/* TODO: WL6049 needs to fix this.
				Column renaming needs to update
				referencing table defintion */
#if 0

				if (foreign_table->foreign_set.empty()) 	{
					MDL_ticket*	mdl = nullptr;
					const dd::Table* dd_f_table = nullptr;
					dd_mdl_acquire(
						thd, &mdl,
						db_name.c_str(),
						(char*)tb_name.c_str());

					if (client->acquire(
						db_name.c_str(),
						tb_name.c_str(),
						&dd_f_table)) {
						continue;
					} else {
						dd_table_load_fk_from_dd(
							foreign_table,
							dd_f_table, nullptr,
							true);
					}
					dd_mdl_release(thd, &mdl);
				}
#endif

				for (auto &fk : foreign_table->foreign_set) {
					if (strcmp(fk->referenced_table_name,
						   tbl_name) != 0) {
						continue;
					}

					if (fk->referenced_table) {
						ut_ad(fk->referenced_table == m_table);
					} else {
						err = dict_foreign_add_to_cache(
							fk, col_names,
							check_charsets,
							DICT_ERR_IGNORE_NONE);
					}
				}
				foreign_table->release();
			} else {
				/* To avoid recursively loading the tables
				related through the foreign key constraints,
				the child table name is saved here. The child
				table will be loaded later, along with its
                                foreign key constraint. */
                                lint    old_size = mem_heap_get_size(
					m_table->heap);

                                fk_tables->push_back(
                                        mem_heap_strdupl(m_table->heap,
                                                full_name,
                                                strlen(full_name)));

                                lint    new_size = mem_heap_get_size(
					m_table->heap);
                                dict_sys->size += new_size - old_size;
                        }

			mutex_exit(&dict_sys->mutex);

			ut_ad(it != child_name.end());
			++it;
		}
	}

	return(err);
}

/** Get tablespace name of dd::Table
@param[in]	dd_table	dd table object
@return the tablespace name. */
template<typename Table>
const char*
dd_table_get_space_name(
	const Table*		dd_table)
{
	dd::Tablespace*		dd_space = nullptr;
	THD*			thd = current_thd;
	const char*		space_name;

	DBUG_ENTER("dd_tablee_get_space_name");
	ut_ad(!srv_is_being_shutdown);

	dd::cache::Dictionary_client*	client = dd::get_dd_client(thd);
	dd::cache::Dictionary_client::Auto_releaser	releaser(client);

	dd::Object_id   dd_space_id = (*dd_table->indexes().begin())
		->tablespace_id();

	if (client->acquire_uncached_uncommitted<dd::Tablespace>(
		dd_space_id, &dd_space)) {
		ut_a(false);
	}

	ut_a(dd_space != NULL);
	space_name = dd_space->name().c_str();

	DBUG_RETURN(space_name);
}

/** Opens a tablespace for dd_load_table_one()
@param[in,out]	dd_table	dd table
@param[in,out]	table		A table that refers to the tablespace to open
@param[in,out]	heap		A memory heap
@param[in]	ignore_err	Whether to ignore an error. */
template<typename Table>
void
dd_load_tablespace(
	const Table*			dd_table,
	dict_table_t*			table,
	mem_heap_t*				heap,
	dict_err_ignore_t		ignore_err)
{
	ut_ad(!table->is_temporary());

	/* The system and temporary tablespaces are preloaded and always available. */
	if (fsp_is_system_or_temp_tablespace(table->space)) {
		return;
	}

	if (table->flags2 & DICT_TF2_DISCARDED) {
		ib::warn() << "Tablespace for table " << table->name
			<< " is set as discarded.";
		table->ibd_file_missing = TRUE;
		return;
	}

	/* A file-per-table table name is also the tablespace name.
	A general tablespace name is not the same as the table name.
	Use the general tablespace name if it can be read from the
	dictionary, if not use 'innodb_general_##. */
	char*	shared_space_name = NULL;
	char*	space_name;
	if (DICT_TF_HAS_SHARED_SPACE(table->flags)) {
		if (table->space == dict_sys_t::space_id) {
			shared_space_name = mem_strdup(
				dict_sys_t::dd_space_name);
		}
		else if (srv_sys_tablespaces_open) {
			shared_space_name = mem_strdup(
				dd_table_get_space_name(dd_table));
		}
		else {
			/* Make the temporary tablespace name. */
			shared_space_name = static_cast<char*>(
				ut_malloc_nokey(
					strlen(general_space_name) + 20));

			sprintf(shared_space_name, "%s_" ULINTPF,
				general_space_name,
				static_cast<ulint>(table->space));
		}
		space_name = shared_space_name;
	}
	else {
		space_name = table->name.m_name;
	}

	/* The tablespace may already be open. */
	if (fil_space_for_table_exists_in_mem(
		table->space, space_name, false,
		true, heap, table->id)) {
		ut_free(shared_space_name);
		return;
	}

	if (!(ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK)) {
		ib::error() << "Failed to find tablespace for table "
			<< table->name << " in the cache. Attempting"
			" to load the tablespace with space id "
			<< table->space;
	}

	/* Use the remote filepath if needed. This parameter is optional
	in the call to fil_ibd_open(). If not supplied, it will be built
	from the space_name. */
	char* filepath = NULL;
	if (DICT_TF_HAS_DATA_DIR(table->flags)) {
		/* This will set table->data_dir_path from either
		fil_system or SYS_DATAFILES */
		dict_get_and_save_data_dir_path(table, true);

		if (table->data_dir_path) {
			filepath = fil_make_filepath(
				table->data_dir_path,
				table->name.m_name, IBD, true);
		}

	}
	else if (DICT_TF_HAS_SHARED_SPACE(table->flags)) {
		/* Set table->tablespace from either
		fil_system or SYS_TABLESPACES */
		dict_get_and_save_space_name(table, true);

		filepath = dict_get_first_path(table->space);
		if (filepath == NULL) {
			ib::warn() << "Could not find the filepath"
				" for table " << table->name <<
				", space ID " << table->space;
		}
	}

	/* Try to open the tablespace.  We set the 2nd param (fix_dict) to
	false because we do not have an x-lock on dict_operation_lock */
	bool is_encrypted = dict_table_is_encrypted(table);
	ulint fsp_flags = dict_tf_to_fsp_flags(table->flags,
		is_encrypted);

	dberr_t err = fil_ibd_open(
		true, FIL_TYPE_TABLESPACE, table->space,
		fsp_flags, space_name, filepath);

	if (err != DB_SUCCESS) {
		/* We failed to find a sensible tablespace file */
		table->ibd_file_missing = TRUE;
	}

	ut_free(shared_space_name);
	ut_free(filepath);
}

/** Open or load a table definition based on a Global DD object.
@tparam		Table		dd::Table or dd::Partition
@param[in,out]	client		data dictionary client
@param[in]	table		MySQL table definition
@param[in]	norm_name	Table Name
@param[out]	ib_table	InnoDB table handle
@param[in]	dd_table	Global DD table or partition object
@param[in]	skip_mdl	whether meta-data locking is skipped
@param[in]	thd		thread THD
@param[in,out]	fk_list		stack of table names which neet to load
@retval 0                       on success
@retval HA_ERR_TABLE_CORRUPT    if the table is marked as corrupted
@retval HA_ERR_TABLESPACE_MISSING       if the file is not found */
template<typename Table>
dict_table_t*
dd_open_table_one(
	dd::cache::Dictionary_client*	client,
	const TABLE*			table,
	const char*			norm_name,
	dict_table_t*&			ib_table,
	const Table*			dd_table,
	bool				skip_mdl,
	THD*				thd,
	dict_names_t&			fk_list)
{
	ut_ad(dd_table != NULL);

	bool	implicit;

	if (dd_table->tablespace_id() == dict_sys_t::dd_space_id
	    || dd_table->tablespace_id() == 10001) {
		/* DD tables are in shared DD tablespace */
		implicit = false;
	} else if (dd_tablespace_is_implicit(
		client, dd_first_index(dd_table)->tablespace_id(),
		&implicit)) {
		/* Tablespace no longer exist, it could be already dropped */
		return(NULL);
	}

	const bool      zip_allowed = srv_page_size <= UNIV_ZIP_SIZE_MAX;
	const bool	strict = false;
	bool		first_index = true;

	/* Create dict_table_t for the table */
	dict_table_t* m_table = dd_fill_dict_table(
		dd_table, table, norm_name,
		NULL, zip_allowed, strict, thd, skip_mdl, implicit);

	if (m_table == nullptr) {
		return(NULL);
	}

	/* Create dict_index_t for the table */
	mutex_enter(&dict_sys->mutex);
	int	ret;
	ret = dd_fill_dict_index(
		dd_table->table(), table, m_table, NULL, zip_allowed,
		strict, thd, skip_mdl);

	mutex_exit(&dict_sys->mutex);

	if (ret != 0) {
		return(NULL);
	}

	mem_heap_t*	heap = mem_heap_create(1000);
	bool		fail = false;

	/* Now fill the space ID and Root page number for each index */
        dict_index_t*	index = m_table->first_index();
        for (const auto dd_index : dd_table->indexes()) {
                ut_ad(index != nullptr);

                const dd::Properties&   se_private_data
                        = dd_index->se_private_data();
                uint64                  id = 0;
                uint32                  root = 0;
		uint32			sid = 0;
		uint64			trx_id = 0;
		dd::Object_id		index_space_id =
			dd_index->tablespace_id();
		dd::Tablespace*	index_space = nullptr;

		if (dd_table->tablespace_id() == dict_sys_t::dd_space_id
		    || dd_table->tablespace_id() == 10001) {
			sid = dict_sys_t::space_id;
		} else if (dd_table->tablespace_id()
			   == dict_sys_t::dd_temp_space_id) {
			sid = dict_sys_t::temp_space_id;
		} else {
			if (client->acquire_uncached_uncommitted<
			    dd::Tablespace>(index_space_id, &index_space)) {
				my_error(ER_TABLESPACE_MISSING, MYF(0),
					 m_table->name.m_name);
				fail = true;
				break;
			}

			if (index_space->se_private_data().get_uint32(
				dd_space_key_strings[DD_SPACE_ID],
				&sid)) {
				fail = true;
				break;
			}
		}

		if (first_index) {
			ut_ad(m_table->space == 0);
			m_table->space = sid;

			mutex_enter(&dict_sys->mutex);
			dd_load_tablespace(dd_table, m_table, heap,
				DICT_ERR_IGNORE_RECOVER_LOCK);
			mutex_exit(&dict_sys->mutex);
			first_index = false;
		}

		if (se_private_data.get_uint64(
			    dd_index_key_strings[DD_INDEX_ID], &id)
		    || se_private_data.get_uint32(
			    dd_index_key_strings[DD_INDEX_ROOT], &root)
		    || se_private_data.get_uint64(
			    dd_index_key_strings[DD_INDEX_TRX_ID], &trx_id)) {
			fail = true;
			break;
		}

                ut_ad(root > 1);
                ut_ad(index->type & DICT_FTS || root != FIL_NULL
			|| dict_table_is_discarded(m_table));
                ut_ad(id != 0);
		index->page = root;
		index->space = sid;
		index->id = id;
		index->trx_id = trx_id;
                index = index->next();
	}

	if (!implicit) {
		dict_get_and_save_space_name(m_table, false);
	}

	if (fail) {
		for (dict_index_t* index = UT_LIST_GET_LAST(m_table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_LAST(m_table->indexes)) {
			dict_index_remove_from_cache(m_table, index);
		}
		dict_mem_table_free(m_table);

		mem_heap_free(heap);
		return(NULL);
	}

	mutex_enter(&dict_sys->mutex);

	/* Re-check if the table has been opened/added by a concurrent
	thread */
	dict_table_t*	exist = dict_table_check_if_in_cache_low(norm_name);
	if (exist != NULL) {
		for (dict_index_t* index = UT_LIST_GET_LAST(m_table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_LAST(m_table->indexes)) {
			dict_index_remove_from_cache(m_table, index);
		}
		dict_mem_table_free(m_table);

		m_table = exist;
	} else {
		dict_table_add_to_cache(m_table, TRUE, heap);

		if (dict_sys->dynamic_metadata != NULL) {
			dict_table_load_dynamic_metadata(m_table);
		}
	}

	m_table->acquire();

	mutex_exit(&dict_sys->mutex);

	/* Load foreign key info. It could also register child table(s) that
	refers to current table */
	if (exist == NULL) {
		dd_table_load_fk(client, norm_name, nullptr,
				 m_table, &dd_table->table(), thd, false,
				 true, &fk_list);
	}
	mem_heap_free(heap);

	return(m_table);
}


/** Open foreign tables reference a table.
@param[in,out]	client		data dictionary client
@param[in]	fk_list		foreign key name list
@param[in]	thd		thread THD */
void
dd_open_fk_tables(
	dd::cache::Dictionary_client*	client,
	dict_names_t&			fk_list,
	bool				dict_locked,
	THD*				thd)
{
	while (!fk_list.empty()) {
		table_name_t		fk_table_name;
		dict_table_t*		fk_table;
		const dd::Table*	dd_table = nullptr;

		fk_table_name.m_name =
			const_cast<char*>(fk_list.front());
		if (!dict_locked) {
			mutex_enter(&dict_sys->mutex);
		}

		fk_table = dict_table_check_if_in_cache_low(
			fk_table_name.m_name);
		if (!dict_locked) {
			mutex_exit(&dict_sys->mutex);
		}

		if (!fk_table) {
			MDL_ticket*     fk_mdl = nullptr;
			char		db_buf[NAME_LEN + 1];
			char		tbl_buf[NAME_LEN + 1];

			if (!innobase_parse_tbl_name(
				fk_table_name.m_name,
				db_buf, tbl_buf, NULL)) {
				goto next;
			}

			dd_mdl_acquire(thd, &fk_mdl, db_buf, tbl_buf);

			if (client->acquire(db_buf, tbl_buf, &dd_table)
			    || dd_table == nullptr) {
				dd_mdl_release(thd, &fk_mdl);
				goto next;
			}

			ut_ad(dd_table->se_private_id()
			      != dd::INVALID_OBJECT_ID);

			TABLE_SHARE	ts;

			init_tmp_table_share(thd,
				&ts, db_buf, strlen(db_buf),
				dd_table->name().c_str(),
				""/* file name */, nullptr);

			ulint error = open_table_def(thd, &ts, false,
						     dd_table);

			if (error) {
				dd_mdl_release(thd, &fk_mdl);
				goto next;
			}

			TABLE	td;

			error = open_table_from_share(thd, &ts,
				dd_table->name().c_str(),
				0, OPEN_FRM_FILE_ONLY, 0,
				&td, false, dd_table);

			if (error) {
				free_table_share(&ts);
				dd_mdl_release(thd, &fk_mdl);
				goto next;
			}

			fk_table = dd_open_table_one(
				client, &td, fk_table_name.m_name,
				fk_table, dd_table,
				false, thd, fk_list);

			closefrm(&td, false);
			free_table_share(&ts);
			dd_table_close(fk_table, thd, &fk_mdl, false);
		}
next:
		fk_list.pop_front();
	}
}

/** Open or load a table definition based on a Global DD object.
@tparam		Table		dd::Table or dd::Partition
@param[in,out]	client		data dictionary client
@param[in]	table		MySQL table definition
@param[in]	norm_name	Table Name
@param[out]	ib_table	InnoDB table handle
@param[in]	dd_table	Global DD table or partition object
@param[in]	skip_mdl	whether meta-data locking is skipped
@param[in]	thd		thread THD
@retval 0			on success
@retval HA_ERR_TABLE_CORRUPT    if the table is marked as corrupted
@retval HA_ERR_TABLESPACE_MISSING       if the file is not found */
template<typename Table>
dict_table_t*
dd_open_table(
	dd::cache::Dictionary_client*	client,
	const TABLE*			table,
	const char*			norm_name,
	dict_table_t*&			ib_table,
	const Table*			dd_table,
	bool				skip_mdl,
	THD*				thd)
{
	dict_table_t*			m_table = NULL;
	dict_names_t			fk_list;

	m_table = dd_open_table_one(client, table, norm_name,
				    ib_table, dd_table, skip_mdl, thd,
				    fk_list);

	/* If there is foreign table references to this table, we will
	try to open them */
	if (m_table != NULL && !fk_list.empty()) {
		dd::cache::Dictionary_client*	client
			= dd::get_dd_client(thd);
		dd::cache::Dictionary_client::Auto_releaser
			releaser(client);

		dd_open_fk_tables(client, fk_list, false, thd);
	}

	return(m_table);
}

template dict_table_t* dd_open_table<dd::Table>(
        dd::cache::Dictionary_client*, const TABLE*, const char*,
	dict_table_t*&, const dd::Table*, bool, THD*);

template dict_table_t* dd_open_table<dd::Partition>(
	dd::cache::Dictionary_client*, const TABLE*, const char*,
	dict_table_t*&, const dd::Partition*, bool, THD*);
