/*****************************************************************************\
 *  accounting_storage_mysql.c - accounting interface to mysql.
 *
 *  $Id: accounting_storage_mysql.c 13061 2008-01-22 21:23:56Z da $
 *****************************************************************************
 *  Copyright (C) 2004-2007 The Regents of the University of California.
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission 
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and 
 *  distribute linked combinations including the two. You must obey the GNU 
 *  General Public License in all respects for all of the code used other than 
 *  OpenSSL. If you modify file(s) with this exception, you may extend this 
 *  exception to your version of the file(s), but you are not obligated to do 
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in 
 *  the program, then also delete it here.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 *****************************************************************************
 * Notes on mysql configuration 
 *	Assumes mysql is installed as user root
 *	Assumes SlurmUser is configured as user slurm
 * # mysqladmin create <db_name>
 *	The <db_name> goes into slurmdbd.conf as StorageLoc
 * # mysql --user=root -p
 * mysql> GRANT ALL ON *.* TO 'slurm'@'localhost' IDENTIFIED BY PASSWORD 'pw';
 * mysql> GRANT SELECT, INSERT ON *.* TO 'slurm'@'localhost';
\*****************************************************************************/

#include <strings.h>
#include "mysql_jobacct_process.h"
#include "src/common/slurmdbd_defs.h"
#include "src/common/slurm_auth.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "accounting_storage" for SLURM job completion
 * logging) and <method>
 * is a description of how this plugin satisfies that application.  SLURM will
 * only load job completion logging plugins if the plugin_type string has a 
 * prefix of "accounting_storage/".
 *
 * plugin_version - an unsigned 32-bit integer giving the version number
 * of the plugin.  If major and minor revisions are desired, the major
 * version number may be multiplied by a suitable magnitude constant such
 * as 100 or 1000.  Various SLURM versions will likely require a certain
 * minimum versions for their plugins as the job accounting API 
 * matures.
 */
const char plugin_name[] = "Accounting storage MYSQL plugin";
const char plugin_type[] = "accounting_storage/mysql";
const uint32_t plugin_version = 100;

#ifdef HAVE_MYSQL

static mysql_db_info_t *mysql_db_info = NULL;
static char *mysql_db_name = NULL;

#define DEFAULT_ACCT_DB "slurm_acct_db"
#define DELETE_SEC_BACK 86400

char *acct_coord_table = "acct_coord_table";
char *acct_table = "acct_table";
char *assoc_day_table = "assoc_day_usage_table";
char *assoc_hour_table = "assoc_hour_usage_table";
char *assoc_month_table = "assoc_month_usage_table";
char *assoc_table = "assoc_table";
char *cluster_day_table = "cluster_day_usage_table";
char *cluster_hour_table = "cluster_hour_usage_table";
char *cluster_month_table = "cluster_month_usage_table";
char *cluster_table = "cluster_table";
char *event_table = "cluster_event_table";
char *job_table = "job_table";
char *step_table = "step_table";
char *txn_table = "txn_table";
char *user_table = "user_table";

extern int acct_storage_p_commit(mysql_conn_t *mysql_conn, bool commit);

extern int acct_storage_p_add_associations(mysql_conn_t *mysql_conn,
					   uint32_t uid, 
					   List association_list);
extern List acct_storage_p_get_associations(mysql_conn_t *mysql_conn, 
					    acct_association_cond_t *assoc_q);

static int _move_account(mysql_conn_t *mysql_conn, uint32_t lft, uint32_t rgt,
			 char *cluster,
			 char *id, char *parent)
{
/*
  tested sql...

  SELECT @parLeft := lft from assoc_table where cluster='name' && acct='new parent' && user='';

  SELECT @oldLeft := lft, @oldRight := rgt, @myWidth := (rgt - lft + 1), @myDiff := (@parLeft+1) - lft FROM assoc_table WHERE id = 'account id';

  update assoc_table set deleted = deleted + 2, lft = lft + @myDiff, rgt = rgt + @myDiff WHERE lft BETWEEN @oldLeft AND @oldRight;

  UPDATE assoc_table SET rgt = rgt + @myWidth WHERE rgt > @parLeft && deleted < 2;
  UPDATE assoc_table SET lft = lft + @myWidth WHERE lft > @parLeft && deleted < 2;

  UPDATE assoc_table SET rgt = rgt - @myWidth WHERE (@myDiff < 0 && rgt > @oldRight && deleted < 2) || (@myDiff >= 0 && rgt > @oldLeft);
  UPDATE assoc_table SET lft = lft - @myWidth WHERE (@myDiff < 0 && lft > @oldRight && deleted < 2) || (@myDiff >= 0 && lft > @oldLeft);

  update assoc_table set deleted = deleted - 2 WHERE deleted > 1;
	   
  update assoc_table set parent_acct='new parent' where id = 'account id';

*/
	int rc = SLURM_SUCCESS;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	uint32_t par_left = 0;
	uint32_t diff = 0;
	uint32_t width = 0;
	char *query = xstrdup_printf(
		"SELECT lft from %s " 
		"where cluster='%s' && acct='%s' && user='';",
		assoc_table,
		cluster, parent);
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return SLURM_ERROR;
	}
	xfree(query);
	if(!(row = mysql_fetch_row(result))) {
		error("no row");
		mysql_free_result(result);
		return SLURM_ERROR;
	}
	par_left = atoi(row[0]);
	mysql_free_result(result);
	
	width = (rgt - lft + 1);
	diff = ((par_left + 1) - lft);
	
	xstrfmtcat(query,
		   "update %s set deleted = deleted + 2, "
		   "lft = lft + %u, rgt = rgt + %u "
		   "WHERE lft BETWEEN %u AND %u;",
		   assoc_table, diff, diff, lft, rgt);

	xstrfmtcat(query,
		   "UPDATE %s SET rgt = rgt + %u WHERE "
		   "rgt > %u && deleted < 2;"
		   "UPDATE %s SET lft = lft + %u WHERE "
		   "lft > %u && deleted < 2;",
		   assoc_table, width,
		   par_left,
		   assoc_table, width,
		   par_left);

	xstrfmtcat(query,
		   "UPDATE %s SET rgt = rgt - %u WHERE "
		   "(%u < 0 && rgt > %u && deleted < 2) "
		   "|| (%u >= 0 && rgt > %u);"
		   "UPDATE %s SET lft = lft - %u WHERE "
		   "(%u < 0 && lft > %u && deleted < 2) "
		   "|| (%u >= 0 && lft > %u);",
		   assoc_table, width,
		   diff, rgt,
		   diff, lft,
		   assoc_table, width,
		   diff, rgt,
		   diff, lft);

	xstrfmtcat(query,
		   "update %s set deleted = deleted - 2 WHERE deleted > 1;",
		   assoc_table);
	xstrfmtcat(query,
		   "update %s set parent_acct='%s' where id = %s;",
		   assoc_table, parent, id);
	debug3("%d query\n%s", mysql_conn->conn, query);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);

	return rc;
}

static int _move_parent(mysql_conn_t *mysql_conn, uint32_t lft, uint32_t rgt,
			char *cluster,
			char *id, char *old_parent, char *new_parent)
{
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	char *query = NULL;
	int rc = SLURM_SUCCESS;

	/* first we need to see if we are
	 * going to make a child of this
	 * account the new parent.  If so we
	 * need to move that child to this
	 * accounts parent and then do the move 
	 */
	query = xstrdup_printf(
		"select id, lft, rgt from %s where lft between %u and %u "
		"&& acct='%s' && user='' order by lft;",
		assoc_table, lft, rgt,
		new_parent);
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = 
	     mysql_db_query_ret(mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return SLURM_ERROR;
	}
	xfree(query);

	if((row = mysql_fetch_row(result))) {
		debug4("%s(%s) %s,%s is a child of %s",
		       new_parent, row[0], row[1], row[2], id);
		rc = _move_account(mysql_conn, atoi(row[1]), atoi(row[2]),
				  cluster, row[0], old_parent);
	}

	mysql_free_result(result);

	if(rc == SLURM_ERROR) 
		return rc;
	
	/* now move the one we wanted to move in the first place */
	rc = _move_account(mysql_conn, lft, rgt, cluster, id, new_parent);

	return rc;
}

/* This function will take the object given and free it later so it
 * needed to be removed from a list if in one before 
 */
static int _addto_update_list(List update_list, acct_update_type_t type,
			      void *object)
{
	acct_update_object_t *update_object = NULL;
	ListIterator itr = NULL;
	if(!update_list) {
		error("no update list given");
		return SLURM_ERROR;
	}

	itr = list_iterator_create(update_list);
	while((update_object = list_next(itr))) {
		if(update_object->type == type)
			break;
	}
	list_iterator_destroy(itr);

	if(update_object) {
		list_append(update_object->objects, object);
		return SLURM_SUCCESS;
	} 
	update_object = xmalloc(sizeof(acct_update_object_t));

	list_append(update_list, update_object);

	update_object->type = type;
	
	switch(type) {
	case ACCT_MODIFY_USER:
	case ACCT_ADD_USER:
	case ACCT_REMOVE_USER:
		update_object->objects = list_create(destroy_acct_user_rec);
		break;
	case ACCT_ADD_ASSOC:
	case ACCT_MODIFY_ASSOC:
	case ACCT_REMOVE_ASSOC:
		update_object->objects = list_create(
			destroy_acct_association_rec);
		break;
	case ACCT_UPDATE_NOTSET:
	default:
		error("unknown type set in update_object: %d", type);
		return SLURM_ERROR;
	}
	list_append(update_object->objects, object);
	return SLURM_SUCCESS;
}

static int _last_affected_rows(MYSQL *mysql_db)
{
	int status=0, rows=0;
	MYSQL_RES *result = NULL;

	do {
		result = mysql_store_result(mysql_db);
		if (result) 
			mysql_free_result(result);
		else 
			if (mysql_field_count(mysql_db) == 0) {
				status = mysql_affected_rows(mysql_db);
				if(status > 0)
					rows = status;
			}
		if ((status = mysql_next_result(mysql_db)) > 0)
			debug3("Could not execute statement\n");
	} while (status == 0);
	
	return rows;
}

static int _modify_common(mysql_conn_t *mysql_conn,
			  uint16_t type,
			  time_t now,
			  char *user_name,
			  char *table,
			  char *cond_char,
			  char *vals) 
{
	char *query = NULL;
	int rc = SLURM_SUCCESS;

	xstrfmtcat(query, 
		   "update %s set mod_time=%d%s "
		   "where deleted=0 && %s;",
		   table, now, vals,
		   cond_char);
	xstrfmtcat(query, 	
		   "insert into %s "
		   "(timestamp, action, name, actor, info) "
		   "values (%d, %u, \"%s\", '%s', \"%s\");",
		   txn_table,
		   now, type, cond_char, user_name, vals);
	debug3("%d query\n%s", mysql_conn->conn, query);		
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);

	if(rc != SLURM_SUCCESS) {
		if(mysql_conn->rollback) {
			mysql_db_rollback(mysql_conn->acct_mysql_db);
		}
		list_flush(mysql_conn->update_list);
		
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}
static int _modify_unset_users(mysql_conn_t *mysql_conn,
			       acct_association_rec_t *assoc,
			       char *acct,
			       uint32_t lft, uint32_t rgt,
			       List ret_list)
{
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	char *query = NULL, *object = NULL;
	int i;

	char *assoc_req_inx[] = {
		"id",
		"user",
		"acct",
		"cluster",
		"partition",
		"max_jobs",
		"max_nodes_per_job",
		"max_wall_duration_per_job",
		"max_cpu_secs_per_job",
		"lft",
		"rgt"
	};
	
	enum {
		ASSOC_ID,
		ASSOC_USER,
		ASSOC_ACCT,
		ASSOC_CLUSTER,
		ASSOC_PART,
		ASSOC_MJ,
		ASSOC_MNPJ,
		ASSOC_MWPJ,
		ASSOC_MCPJ,
		ASSOC_LFT,
		ASSOC_RGT,
		ASSOC_COUNT
	};

	if(!ret_list || !acct)
		return SLURM_ERROR;

	for(i=0; i<ASSOC_COUNT; i++) {
		if(i) 
			xstrcat(object, ", ");
		xstrcat(object, assoc_req_inx[i]);
	}

	query = xstrdup_printf("select distinct %s from %s where deleted=0 "
			       "&& lft between %u and %u && "
			       "((user = '' && parent_acct = '%s') || "
			       "(user != '' && acct = '%s')) "
			       "order by lft;",
			       object, assoc_table, lft, rgt, acct, acct);
/* 	query = xstrdup_printf("select distinct %s from %s where deleted=0 " */
/* 			       "&& lft between %u and %u and user != ''" */
/* 			       "order by lft;", */
/* 			       object, assoc_table, lft, rgt); */
	xfree(object);
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result =
	     mysql_db_query_ret(mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return SLURM_ERROR;
	}
	xfree(query);

	while((row = mysql_fetch_row(result))) {
		acct_association_rec_t *mod_assoc = NULL;
		int modified = 0;

		mod_assoc = xmalloc(sizeof(acct_association_rec_t));
		mod_assoc->id = atoi(row[ASSOC_ID]);

		if(!row[ASSOC_MJ] && assoc->max_jobs != (uint32_t)NO_VAL) {
			mod_assoc->max_jobs = assoc->max_jobs;
			modified = 1;
		} else
			mod_assoc->max_jobs = (uint32_t)NO_VAL;
		
		if(!row[ASSOC_MNPJ] &&
		   assoc->max_nodes_per_job != (uint32_t)NO_VAL) {
			mod_assoc->max_nodes_per_job =
				assoc->max_nodes_per_job;
			modified = 1;
		} else 
			mod_assoc->max_nodes_per_job = (uint32_t)NO_VAL;

		
		if(!row[ASSOC_MWPJ] && 
		   assoc->max_wall_duration_per_job != (uint32_t)NO_VAL) {
			mod_assoc->max_wall_duration_per_job =
				assoc->max_wall_duration_per_job;
			modified = 1;
		} else 
			mod_assoc->max_wall_duration_per_job = (uint32_t)NO_VAL;
					
		if(!row[ASSOC_MCPJ] && 
		   assoc->max_cpu_secs_per_job != (uint32_t)NO_VAL) {
			mod_assoc->max_cpu_secs_per_job = 
				assoc->max_cpu_secs_per_job;
			modified = 1;
		} else
			mod_assoc->max_cpu_secs_per_job = (uint32_t)NO_VAL;
		

		if(modified) {
			if(!row[ASSOC_USER][0]) {
				_modify_unset_users(mysql_conn,
						    mod_assoc,
						    row[ASSOC_ACCT],
						    atoi(row[ASSOC_LFT]),
						    atoi(row[ASSOC_RGT]),
						    ret_list);
				destroy_acct_association_rec(mod_assoc);
				continue;
			}

			mod_assoc->fairshare = (uint32_t)NO_VAL;
			if(row[ASSOC_PART][0]) { 
				// see if there is a partition name
				object = xstrdup_printf(
					"C = %-10s A = %-20s U = %-9s P = %s",
					row[ASSOC_CLUSTER], row[ASSOC_ACCT],
					row[ASSOC_USER], row[ASSOC_PART]);
			} else {
				object = xstrdup_printf(
					"C = %-10s A = %-20s U = %-9s",
					row[ASSOC_CLUSTER], 
					row[ASSOC_ACCT], 
					row[ASSOC_USER]);
			}
			
			list_append(ret_list, object);
			
			if(_addto_update_list(mysql_conn->update_list, 
					      ACCT_MODIFY_ASSOC,
					      mod_assoc) != SLURM_SUCCESS) 
				error("couldn't add to the update list");
		} else {
			xfree(mod_assoc);
		}
	}
	mysql_free_result(result);

	return SLURM_SUCCESS;
}



/* Every option in assoc_char should have a 't1.' infront of it. */
static int _remove_common(mysql_conn_t *mysql_conn,
			  uint16_t type,
			  time_t now,
			  char *user_name,
			  char *table,
			  char *name_char,
			  char *assoc_char) 
{
	int rc = SLURM_SUCCESS;
	char *query = NULL;
	char *loc_assoc_char = NULL;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	time_t day_old = now - DELETE_SEC_BACK;

	/* we want to remove completely all that is less than a day old */
	if(table != assoc_table) {
		query = xstrdup_printf("delete from %s where creation_time>%d "
				       "&& (%s);",
				       table, day_old, name_char);
	}

	xstrfmtcat(query,
		   "update %s set mod_time=%d, deleted=1 "
		   "where deleted=0 && (%s);",
		   table, now, name_char);
	xstrfmtcat(query, 	
		   "insert into %s (timestamp, action, name, actor) "
		   "values (%d, %u, \"%s\", '%s');",
		   txn_table,
		   now, type, name_char, user_name);

	debug3("%d query\n%s", mysql_conn->conn, query);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);
	if(rc != SLURM_SUCCESS) {
		if(mysql_conn->rollback) {
			mysql_db_rollback(mysql_conn->acct_mysql_db);
		}
		list_flush(mysql_conn->update_list);
		
		return SLURM_ERROR;
	}

	/* mark deleted=1 or remove completely the
	   accounting tables
	*/
	if(table != assoc_table) {
		if(!assoc_char) {
			error("no assoc_char");
			if(mysql_conn->rollback) {
				mysql_db_rollback(mysql_conn->acct_mysql_db);
			}
			list_destroy(mysql_conn->update_list);
			mysql_conn->update_list =
				list_create(destroy_acct_update_object);
			return SLURM_ERROR;
		}

		/* If we are doing this on an assoc_table we have
		   already done this, so don't */
/* 		query = xstrdup_printf("select lft, rgt " */
/* 				       "from %s as t2 where %s order by lft;", */
/* 				       assoc_table, assoc_char); */
		query = xstrdup_printf("select distinct t1.id "
				       "from %s as t1, %s as t2 "
				       "where %s && t1.lft between "
				       "t2.lft and t2.rgt;",
				       assoc_table, assoc_table, assoc_char);
		
		debug3("%d query\n%s", mysql_conn->conn, query);
		if(!(result = mysql_db_query_ret(
			     mysql_conn->acct_mysql_db, query, 0))) {
			xfree(query);
			if(mysql_conn->rollback) {
				mysql_db_rollback(mysql_conn->acct_mysql_db);
			}
			list_destroy(mysql_conn->update_list);
			mysql_conn->update_list =
				list_create(destroy_acct_update_object);
			return SLURM_ERROR;
		}
		xfree(query);

		rc = 0;
		loc_assoc_char = NULL;
		while((row = mysql_fetch_row(result))) {
			acct_association_rec_t *rem_assoc = NULL;
			if(!rc) {
				xstrfmtcat(loc_assoc_char, "id=%s", row[0]);
				rc = 1;
			} else {
				xstrfmtcat(loc_assoc_char,
					   " || id=%s", row[0]);
			}
			rem_assoc = xmalloc(sizeof(acct_association_rec_t));
			rem_assoc->id = atoi(row[0]);
			if(_addto_update_list(mysql_conn->update_list, 
					      ACCT_REMOVE_ASSOC,
					      rem_assoc) != SLURM_SUCCESS) 
				error("couldn't add to the update list");
		}
		mysql_free_result(result);
	} else 
		loc_assoc_char = assoc_char;

/* 	query = xstrdup_printf( */
/* 		"delete t2 from %s as t2, %s as t1 where t1.creation_time>%d && (%s);" */
/* 		"delete t2 from %s as t2, %s as t1 where t1.creation_time>%d && (%s);" */
/* 		"delete t2 from %s as t2, %s as t1 where t1.creation_time>%d && (%s);", */
/* 		assoc_day_table, assoc_table, day_old, loc_assoc_char, */
/* 		assoc_hour_table, assoc_table, day_old, loc_assoc_char, */
/* 		assoc_month_table, assoc_table, day_old, loc_assoc_char); */
	query = xstrdup_printf(
		"delete from %s where creation_time>%d && (%s);"
		"delete from %s where creation_time>%d && (%s);"
		"delete from %s where creation_time>%d && (%s);",
		assoc_day_table, day_old, loc_assoc_char,
		assoc_hour_table, day_old, loc_assoc_char,
		assoc_month_table, day_old, loc_assoc_char);
	xstrfmtcat(query,
		   "update %s set mod_time=%d, deleted=1 where (%s);"
		   "update %s set mod_time=%d, deleted=1 where (%s);"
		   "update %s set mod_time=%d, deleted=1 where (%s);",
		   assoc_day_table, now, loc_assoc_char,
		   assoc_hour_table, now, loc_assoc_char,
		   assoc_month_table, now, loc_assoc_char);

	debug3("%d query\n%s %d", mysql_conn->conn, query, strlen(query));
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);
	if(rc != SLURM_SUCCESS) {
		if(mysql_conn->rollback) {
			mysql_db_rollback(mysql_conn->acct_mysql_db);
		}
		list_flush(mysql_conn->update_list);
		return SLURM_ERROR;
	}

	/* remove completely all the associations for this added in the last
	 * day, since they are most likely nothing we really wanted in
	 * the first place.
	 */
	query = xstrdup_printf("select id from %s as t1 where "
			       "creation_time>%d && (%s);",
			       assoc_table, day_old, loc_assoc_char);
	
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		if(mysql_conn->rollback) {
			mysql_db_rollback(mysql_conn->acct_mysql_db);
		}
		list_flush(mysql_conn->update_list);
		return SLURM_ERROR;
	}
	xfree(query);

	/* we have to do this one at a time since the lft's and rgt's
	   change */
	while((row = mysql_fetch_row(result))) {
		MYSQL_RES *result2 = NULL;
		MYSQL_ROW row2;
		
		xstrfmtcat(query,
			   "SELECT lft, rgt, (rgt - lft + 1) "
			   "FROM %s WHERE id = %s;",
			   assoc_table, row[0]);
		debug3("%d query\n%s", mysql_conn->conn, query);
		if(!(result2 = mysql_db_query_ret(
			     mysql_conn->acct_mysql_db, query, 0))) {
			xfree(query);
			rc = SLURM_ERROR;
			break;
		}
		xfree(query);
		if(!(row2 = mysql_fetch_row(result2))) {
			mysql_free_result(result2);
			continue;
		}

		xstrfmtcat(query,
			   "delete quick from %s where lft between "
			   "%s AND %s;",
			   assoc_table,
			   row2[0], row2[1]);
		xstrfmtcat(query,
			   "UPDATE %s SET rgt = rgt - %s WHERE "
			   "rgt > %s;"
			   "UPDATE %s SET lft = lft - %s WHERE "
			   "lft > %s;",
			   assoc_table, row2[2],
			   row2[1],
			   assoc_table, row2[2], row2[1]);

		mysql_free_result(result2);

		debug3("%d query\n%s", mysql_conn->conn, query);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
		xfree(query);
		if(rc != SLURM_SUCCESS) {
			error("couldn't remove assoc");
			break;
		}
	}
	mysql_free_result(result);
	if(rc == SLURM_ERROR) {
		if(mysql_conn->rollback) {
			mysql_db_rollback(mysql_conn->acct_mysql_db);
		}
		list_flush(mysql_conn->update_list);
		return rc;
	}
	
	if(table == assoc_table)
		return SLURM_SUCCESS;
	
	/* now update the associations themselves that are still around */
	query = xstrdup_printf("update %s as t1 set mod_time=%d, deleted=1 "
			       "where deleted=0 && (%s);",
			       assoc_table, now,
			       loc_assoc_char);
	xfree(loc_assoc_char);
	debug3("%d query\n%s", mysql_conn->conn, query);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);
	if(rc != SLURM_SUCCESS) {
		if(mysql_conn->rollback) {
			mysql_db_rollback(mysql_conn->acct_mysql_db);
		}
		list_flush(mysql_conn->update_list);
	}
	
	return rc;
}

static int _get_db_index(MYSQL *acct_mysql_db, 
			 time_t submit, uint32_t jobid, uint32_t associd)
{
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	int db_index = -1;
	char *query = xstrdup_printf("select id from %s where "
				     "submit=%u and jobid=%u and associd=%u",
				     job_table, (int)submit, jobid, associd);

	if(!(result = mysql_db_query_ret(acct_mysql_db, query, 0))) {
		xfree(query);
		return -1;
	}
	xfree(query);

	row = mysql_fetch_row(result);
	if(!row) {
		mysql_free_result(result);
		error("We can't get a db_index for this combo, "
		      "submit=%u and jobid=%u and associd=%u.",
		      (int)submit, jobid, associd);
		return -1;
	}
	db_index = atoi(row[0]);
	mysql_free_result(result);
	
	return db_index;
}

static mysql_db_info_t *_mysql_acct_create_db_info()
{
	mysql_db_info_t *db_info = xmalloc(sizeof(mysql_db_info_t));
	db_info->port = slurm_get_accounting_storage_port();
	if(!db_info->port) 
		db_info->port = 3306;
	db_info->host = slurm_get_accounting_storage_host();	
	db_info->user = slurm_get_accounting_storage_user();	
	db_info->pass = slurm_get_accounting_storage_pass();	
	return db_info;
}

static int _mysql_acct_check_tables(MYSQL *acct_mysql_db)
{
	int rc = SLURM_SUCCESS;
	storage_field_t acct_coord_table_fields[] = {
		{ "deleted", "tinyint default 0" },
		{ "acct", "tinytext not null" },
		{ "user", "tinytext not null" },
		{ NULL, NULL}		
	};

	storage_field_t acct_table_fields[] = {
		{ "creation_time", "int unsigned not null" },
		{ "mod_time", "int unsigned default 0 not null" },
		{ "deleted", "tinyint default 0" },
		{ "name", "tinytext not null" },
		{ "description", "text not null" },
		{ "organization", "text not null" },
		{ "qos", "smallint default 1 not null" },
		{ NULL, NULL}		
	};

	storage_field_t assoc_table_fields[] = {
		{ "creation_time", "int unsigned not null" },
		{ "mod_time", "int unsigned default 0 not null" },
		{ "deleted", "tinyint default 0" },
		{ "id", "int not null auto_increment" },
		{ "user", "tinytext not null default ''" },
		{ "acct", "tinytext not null" },
		{ "cluster", "tinytext not null" },
		{ "partition", "tinytext not null default ''" },
		{ "parent_acct", "tinytext not null default ''" },
		{ "lft", "int not null" },
		{ "rgt", "int not null" },
		{ "fairshare", "int default 1 not null" },
		{ "max_jobs", "int default NULL" },
		{ "max_nodes_per_job", "int default NULL" },
		{ "max_wall_duration_per_job", "int default NULL" },
		{ "max_cpu_secs_per_job", "int default NULL" },
		{ NULL, NULL}		
	};

	storage_field_t assoc_usage_table_fields[] = {
		{ "creation_time", "int unsigned not null" },
		{ "mod_time", "int unsigned default 0 not null" },
		{ "deleted", "tinyint default 0" },
		{ "id", "int not null" },
		{ "period_start", "int unsigned not null" },
		{ "cpu_count", "int unsigned default 0" },
		{ "alloc_cpu_secs", "bigint default 0" },
		{ NULL, NULL}		
	};

	storage_field_t cluster_table_fields[] = {
		{ "creation_time", "int unsigned not null" },
		{ "mod_time", "int unsigned default 0 not null" },
		{ "deleted", "tinyint default 0" },
		{ "name", "tinytext not null" },
		{ "control_host", "tinytext not null default ''" },
		{ "control_port", "mediumint not null default 0" },
		{ NULL, NULL}		
	};

	storage_field_t cluster_usage_table_fields[] = {
		{ "creation_time", "int unsigned not null" },
		{ "mod_time", "int unsigned default 0 not null" },
		{ "deleted", "tinyint default 0" },
		{ "cluster", "tinytext not null" },
		{ "period_start", "int unsigned not null" },
		{ "cpu_count", "int default 0" },
		{ "alloc_cpu_secs", "bigint default 0" },
		{ "down_cpu_secs", "bigint default 0" },
		{ "idle_cpu_secs", "bigint default 0" },
		{ "resv_cpu_secs", "bigint default 0" },
		{ NULL, NULL}		
	};

	storage_field_t event_table_fields[] = {
		{ "node_name", "tinytext default '' not null" },
		{ "cluster", "tinytext not null" },
		{ "cpu_count", "int not null" },
		{ "period_start", "int unsigned not null" },
		{ "period_end", "int unsigned default 0 not null" },
		{ "reason", "tinytext not null" },
		{ NULL, NULL}		
	};

	storage_field_t job_table_fields[] = {
		{ "id", "int not null auto_increment" },
		{ "jobid", "mediumint unsigned not null" },
		{ "associd", "mediumint unsigned not null" },
		{ "uid", "smallint unsigned not null" },
		{ "gid", "smallint unsigned not null" },
		{ "partition", "tinytext not null" },
		{ "blockid", "tinytext" },
		{ "account", "tinytext" },
		{ "eligible", "int unsigned default 0 not null" },
		{ "submit", "int unsigned default 0 not null" },
		{ "start", "int unsigned default 0 not null" },
		{ "end", "int unsigned default 0 not null" },
		{ "suspended", "int unsigned default 0 not null" },
		{ "name", "tinytext not null" }, 
		{ "track_steps", "tinyint not null" },
		{ "state", "smallint not null" }, 
		{ "comp_code", "int default 0 not null" },
		{ "priority", "int unsigned not null" },
		{ "req_cpus", "mediumint unsigned not null" }, 
		{ "alloc_cpus", "mediumint unsigned not null" }, 
		{ "nodelist", "text" },
		{ "kill_requid", "smallint default -1 not null" },
		{ "qos", "smallint default 0" },
		{ NULL, NULL}
	};

	storage_field_t step_table_fields[] = {
		{ "id", "int not null" },
		{ "stepid", "smallint not null" },
		{ "start", "int unsigned default 0 not null" },
		{ "end", "int unsigned default 0 not null" },
		{ "suspended", "int unsigned default 0 not null" },
		{ "name", "text not null" },
		{ "nodelist", "text not null" },
		{ "state", "smallint not null" },
		{ "kill_requid", "smallint default -1 not null" },
		{ "comp_code", "int default 0 not null" },
		{ "cpus", "mediumint unsigned not null" },
		{ "user_sec", "int unsigned default 0 not null" },
		{ "user_usec", "int unsigned default 0 not null" },
		{ "sys_sec", "int unsigned default 0 not null" },
		{ "sys_usec", "int unsigned default 0 not null" },
		{ "max_vsize", "mediumint unsigned default 0 not null" },
		{ "max_vsize_task", "smallint unsigned default 0 not null" },
		{ "max_vsize_node", "mediumint unsigned default 0 not null" },
		{ "ave_vsize", "float default 0.0 not null" },
		{ "max_rss", "mediumint unsigned default 0 not null" },
		{ "max_rss_task", "smallint unsigned default 0 not null" },
		{ "max_rss_node", "mediumint unsigned default 0 not null" },
		{ "ave_rss", "float default 0.0 not null" },
		{ "max_pages", "mediumint unsigned default 0 not null" },
		{ "max_pages_task", "smallint unsigned default 0 not null" },
		{ "max_pages_node", "mediumint unsigned default 0 not null" },
		{ "ave_pages", "float default 0.0 not null" },
		{ "min_cpu", "mediumint unsigned default 0 not null" },
		{ "min_cpu_task", "smallint unsigned default 0 not null" },
		{ "min_cpu_node", "mediumint unsigned default 0 not null" },
		{ "ave_cpu", "float default 0.0 not null" },
		{ NULL, NULL}
	};

	storage_field_t txn_table_fields[] = {
		{ "id", "int not null auto_increment" },
		{ "timestamp", "int unsigned default 0 not null" },
		{ "action", "smallint not null" },
		{ "name", "tinytext not null" },
		{ "actor", "tinytext not null" },
		{ "info", "text" },
		{ NULL, NULL}		
	};

	storage_field_t user_table_fields[] = {
		{ "creation_time", "int unsigned not null" },
		{ "mod_time", "int unsigned default 0 not null" },
		{ "deleted", "bool default 0" },
		{ "name", "tinytext not null" },
		{ "default_acct", "tinytext not null" },
		{ "qos", "smallint default 1 not null" },
		{ "admin_level", "smallint default 1 not null" },
		{ NULL, NULL}		
	};

	char *get_parent_proc = 
		"drop procedure if exists get_parent_limits; "
		"create procedure get_parent_limits("
		"my_table text, acct text, cluster text) "
		"begin "
		"set @par_id = NULL; "
		"set @mj = NULL; "
		"set @mnpj = NULL; "
		"set @mwpj = NULL; "
		"set @mcpj = NULL; "
		"set @my_acct = acct; "
		"REPEAT "
		"set @s = 'select '; "
		"if @par_id is NULL then set @s = CONCAT("
		"@s, '@par_id := id, '); "
		"end if; "
		"if @mj is NULL then set @s = CONCAT("
		"@s, '@mj := max_jobs, '); "
		"end if; "
		"if @mnpj is NULL then set @s = CONCAT("
		"@s, '@mnpj := max_nodes_per_job, ') ;"
		"end if; "
		"if @mwpj is NULL then set @s = CONCAT("
		"@s, '@mwpj := max_wall_duration_per_job, '); "
		"end if; "
		"if @mcpj is NULL then set @s = CONCAT("
		"@s, '@mcpj := max_cpu_secs_per_job, '); "
		"end if; "
		"set @s = concat(@s, ' @my_acct := parent_acct from ', "
		"my_table, ' where acct = \"', @my_acct, '\" && "
		"cluster = \"', cluster, '\" && user=\"\"'); "
		"prepare query from @s; "
		"execute query; "
		"deallocate prepare query; "
		"UNTIL (@mj != -1 && @mnpj != -1 && @mwpj != -1 "
		"&& @mcpj != -1) || @my_acct = '' || "
		"@my_acct = 'root' END REPEAT; "
		"END;";
	
	if(mysql_db_create_table(acct_mysql_db, acct_coord_table,
				 acct_coord_table_fields,
				 ", primary key (acct(20), user(20)))")
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, acct_table, acct_table_fields,
				 ", primary key (name(20)))") == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, assoc_day_table,
				 assoc_usage_table_fields,
				 ", primary key (id, period_start))")
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, assoc_hour_table,
				 assoc_usage_table_fields,
				 ", primary key (id, period_start))")
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, assoc_month_table,
				 assoc_usage_table_fields,
				 ", primary key (id, period_start))") 
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, assoc_table, assoc_table_fields,
				 ", primary key (id), "
				 " unique index (user(20), acct(20), "
				 "cluster(20), partition(20)))"
/* 				 " unique index (lft), " */
/* 				 " unique index (rgt))" */)
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, cluster_day_table,
				 cluster_usage_table_fields,
				 ", primary key (cluster(20), period_start))")
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, cluster_hour_table,
				 cluster_usage_table_fields,
				 ", primary key (cluster(20), period_start))")
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, cluster_month_table,
				 cluster_usage_table_fields,
				 ", primary key (cluster(20), period_start))")
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, cluster_table,
				 cluster_table_fields,
				 ", primary key (name(20)))") == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, event_table,
				 event_table_fields,
				 ", primary key (node_name(20), cluster(20), "
				 "period_start))") == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, job_table, job_table_fields,
				 ", primary key (id), "
				 "unique index (jobid, associd, submit))")
	   == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, step_table,
				 step_table_fields, 
				 ", primary key (id, stepid))") == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, txn_table, txn_table_fields,
				 ", primary key (id))") == SLURM_ERROR)
		return SLURM_ERROR;

	if(mysql_db_create_table(acct_mysql_db, user_table, user_table_fields,
				 ", primary key (name(20)))") == SLURM_ERROR)
		return SLURM_ERROR;

	rc = mysql_db_query(acct_mysql_db, get_parent_proc);

	return rc;
}
#endif

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init ( void )
{
	static int first = 1;
	int rc = SLURM_SUCCESS;
#ifdef HAVE_MYSQL
	MYSQL *acct_mysql_db = NULL;
	char *location = NULL;
#else
	fatal("No MySQL database was found on the machine. "
	      "Please check the configure log and run again.");
#endif

	/* since this can be loaded from many different places
	   only tell us once. */
	if(!first)
		return SLURM_SUCCESS;

	first = 0;

#ifdef HAVE_MYSQL
	mysql_db_info = _mysql_acct_create_db_info();

	location = slurm_get_accounting_storage_loc();
	if(!location)
		mysql_db_name = xstrdup(DEFAULT_ACCT_DB);
	else {
		int i = 0;
		while(location[i]) {
			if(location[i] == '.' || location[i] == '/') {
				debug("%s doesn't look like a database "
				      "name using %s",
				      location, DEFAULT_ACCT_DB);
				break;
			}
			i++;
		}
		if(location[i]) {
			mysql_db_name = xstrdup(DEFAULT_ACCT_DB);
			xfree(location);
		} else
			mysql_db_name = location;
	}

	debug2("mysql_connect() called for db %s", mysql_db_name);
	
	mysql_get_db_connection(&acct_mysql_db, mysql_db_name, mysql_db_info);
		
	rc = _mysql_acct_check_tables(acct_mysql_db);

	mysql_close_db_connection(&acct_mysql_db);
#endif		

	if(rc == SLURM_SUCCESS)
		verbose("%s loaded", plugin_name);
	else 
		verbose("%s failed", plugin_name);
	
	return rc;
}

extern int fini ( void )
{
#ifdef HAVE_MYSQL
	destroy_mysql_db_info(mysql_db_info);		
	xfree(mysql_db_name);
	mysql_cleanup();
	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern void *acct_storage_p_get_connection(bool make_agent, bool rollback)
{
#ifdef HAVE_MYSQL
	mysql_conn_t *mysql_conn = xmalloc(sizeof(mysql_conn_t));
	static int conn = 0;
	if(!mysql_db_info)
		init();

	debug2("acct_storage_p_get_connection: request new connection");
	
	mysql_get_db_connection(&mysql_conn->acct_mysql_db,
				mysql_db_name, mysql_db_info);
	mysql_conn->rollback = rollback;
	if(rollback) {
		mysql_autocommit(mysql_conn->acct_mysql_db, 0);
	}
	mysql_conn->conn = conn++;
	mysql_conn->update_list = list_create(destroy_acct_update_object);
	return (void *)mysql_conn;
#else
	return NULL;
#endif
}

extern int acct_storage_p_close_connection(mysql_conn_t **mysql_conn)
{
#ifdef HAVE_MYSQL

	if(!mysql_conn || !(*mysql_conn))
		return SLURM_SUCCESS;

	acct_storage_p_commit((*mysql_conn), 0);
	mysql_close_db_connection(&(*mysql_conn)->acct_mysql_db);
	list_destroy((*mysql_conn)->update_list);
	xfree((*mysql_conn));

	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern int acct_storage_p_commit(mysql_conn_t *mysql_conn, bool commit)
{
#ifdef HAVE_MYSQL

	if(!mysql_conn) 
		return SLURM_ERROR;

	debug4("got %d commits", list_count(mysql_conn->update_list));
	
	if(mysql_conn->rollback) {
		if(!commit) {
			if(mysql_db_rollback(mysql_conn->acct_mysql_db))
				error("rollback failed");
		} else {
			if(mysql_db_commit(mysql_conn->acct_mysql_db))
				error("commit failed");
		}
	}
	
	if(commit && list_count(mysql_conn->update_list)) {
		int rc;
		char *query = NULL;
		MYSQL_RES *result = NULL;
		MYSQL_ROW row;
		accounting_update_msg_t msg;
		slurm_msg_t req;
		slurm_msg_t resp;
		ListIterator itr = NULL;
		acct_update_object_t *object = NULL;
		
		slurm_msg_t_init(&req);
		slurm_msg_t_init(&resp);
		
		memset(&msg, 0, sizeof(accounting_update_msg_t));
		msg.update_list = mysql_conn->update_list;
		
		xstrfmtcat(query, "select control_host, control_port from %s "
			   "where deleted=0 && control_port != 0",
			   cluster_table);
		if(!(result = mysql_db_query_ret(
			     mysql_conn->acct_mysql_db, query, 0))) {
			xfree(query);
			goto skip;
		}
		xfree(query);
		while((row = mysql_fetch_row(result))) {
			//info("sending to %s(%s)", row[0], row[1]);
			slurm_set_addr_char(&req.address, atoi(row[1]), row[0]);
			req.msg_type = ACCOUNTING_UPDATE_MSG;
			req.flags = SLURM_GLOBAL_AUTH_KEY;
			req.data = &msg;			
			
			rc = slurm_send_recv_node_msg(&req, &resp, 0);
			if ((rc != 0) || !resp.auth_cred) {
				error("update cluster: %m to %s(%s)",
				      row[0], row[1]);
				if (resp.auth_cred)
					g_slurm_auth_destroy(resp.auth_cred);
				rc = SLURM_ERROR;
			}
			if (resp.auth_cred)
				g_slurm_auth_destroy(resp.auth_cred);
			
			switch (resp.msg_type) {
			case RESPONSE_SLURM_RC:
				rc = ((return_code_msg_t *)resp.data)->
					return_code;
				slurm_free_return_code_msg(resp.data);	
				break;
			default:
				break;
			}	
			//info("got rc of %d", rc);
		}
		mysql_free_result(result);
	skip:
		/* NOTE: you can not use list_pop, or list_push
		   anywhere either, since mysql is
		   exporting something of the same type as a macro,
		   which messes everything up (my_list.h is the bad boy).
		   So we are just going to delete each item as it
		   comes out.
		*/
		itr = list_iterator_create(mysql_conn->update_list);
		while((object = list_next(itr))) {
			if(!object->objects || !list_count(object->objects)) {
				list_delete_item(itr);
				continue;
			}
			switch(object->type) {
			case ACCT_MODIFY_USER:
			case ACCT_ADD_USER:
			case ACCT_REMOVE_USER:
				rc = assoc_mgr_update_local_users(object);
				break;
			case ACCT_ADD_ASSOC:
			case ACCT_MODIFY_ASSOC:
			case ACCT_REMOVE_ASSOC:
				rc = assoc_mgr_update_local_assocs(object);
				break;
			case ACCT_UPDATE_NOTSET:
			default:
				error("unknown type set in "
				      "update_object: %u",
				      object->type);
				break;
			}
			list_delete_item(itr);
		}
		list_iterator_destroy(itr);
	}
	list_flush(mysql_conn->update_list);

	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern int acct_storage_p_add_users(mysql_conn_t *mysql_conn, uint32_t uid,
				    List user_list)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;
	acct_user_rec_t *object = NULL;
	char *cols = NULL, *vals = NULL, *query = NULL, *txn_query = NULL;
	struct passwd *pw = NULL;
	time_t now = time(NULL);
	char *user = NULL;
	char *extra = NULL;
	int affect_rows = 0;
	List assoc_list = list_create(destroy_acct_association_rec);

	if((pw=getpwuid(uid))) {
		user = pw->pw_name;
	}

	itr = list_iterator_create(user_list);
	while((object = list_next(itr))) {
		if(!object->name || !object->default_acct) {
			error("We need a user name and "
			      "default acct to add.");
			rc = SLURM_ERROR;
			continue;
		}
		xstrcat(cols, "creation_time, mod_time, name, default_acct");
		xstrfmtcat(vals, "%d, %d, '%s', '%s'", 
			   now, now, object->name, object->default_acct); 
		xstrfmtcat(extra, ", default_acct='%s'", object->default_acct);
		if(object->qos != ACCT_QOS_NOTSET) {
			xstrcat(cols, ", qos");
			xstrfmtcat(vals, ", %u", object->qos); 		
			xstrfmtcat(extra, ", qos=%u", object->qos); 		
		}

		if(object->admin_level != ACCT_ADMIN_NOTSET) {
			xstrcat(cols, ", admin_level");
			xstrfmtcat(vals, ", %u", object->admin_level);
		}

		query = xstrdup_printf(
			"insert into %s (%s) values (%s) "
			"on duplicate key update deleted=0, mod_time=%d %s;",
			user_table, cols, vals,
			now, extra);

		xfree(cols);
		xfree(vals);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
		xfree(query);
		if(rc != SLURM_SUCCESS) {
			error("Couldn't add user %s", object->name);
			xfree(extra);
			continue;
		}

		affect_rows = _last_affected_rows(mysql_conn->acct_mysql_db);
		if(!affect_rows) {
			debug("nothing changed");
			xfree(extra);
			continue;
		}

		if(_addto_update_list(mysql_conn->update_list, ACCT_ADD_USER,
				      object) == SLURM_SUCCESS) 
			list_remove(itr);
			

		if(txn_query)
			xstrfmtcat(txn_query, 	
				   ", (%d, %u, '%s', '%s', \"%s\")",
				   now, DBD_ADD_USERS, object->name,
				   user, extra);
		else
			xstrfmtcat(txn_query, 	
				   "insert into %s "
				   "(timestamp, action, name, actor, info) "
				   "values (%d, %u, '%s', '%s', \"%s\")",
				   txn_table,
				   now, DBD_ADD_USERS, object->name,
				   user, extra);
		xfree(extra);
		
		if(!object->assoc_list)
			continue;

		list_transfer(assoc_list, object->assoc_list);
	}
	list_iterator_destroy(itr);

	if(rc != SLURM_ERROR) {
		if(txn_query) {
			xstrcat(txn_query, ";");
			rc = mysql_db_query(mysql_conn->acct_mysql_db,
					    txn_query);
			xfree(txn_query);
			if(rc != SLURM_SUCCESS) {
				error("Couldn't add txn");
				rc = SLURM_SUCCESS;
			}
		}
	} else
		xfree(txn_query);

	if(list_count(assoc_list)) {
		if(acct_storage_p_add_associations(mysql_conn, uid, assoc_list)
		   == SLURM_ERROR) {
			error("Problem adding user associations");
			rc = SLURM_ERROR;
		}
	}
	list_destroy(assoc_list);

	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int acct_storage_p_add_coord(mysql_conn_t *mysql_conn, uint32_t uid, 
				    char *acct, acct_user_cond_t *user_q)
{
#ifdef HAVE_MYSQL
	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

extern int acct_storage_p_add_accts(mysql_conn_t *mysql_conn, uint32_t uid, 
				    List acct_list)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;
	acct_account_rec_t *object = NULL;
	char *cols = NULL, *vals = NULL, *query = NULL, *txn_query = NULL;
	struct passwd *pw = NULL;
	time_t now = time(NULL);
	char *user = NULL;
	char *extra = NULL;
	int affect_rows = 0;
	List assoc_list = list_create(destroy_acct_association_rec);

	if((pw=getpwuid(uid))) {
		user = pw->pw_name;
	}

	itr = list_iterator_create(acct_list);
	while((object = list_next(itr))) {
		if(!object->name || !object->description
		   || !object->organization) {
			error("We need an account name, description, and "
			      "organization to add. %s %s %s", 
			      object->name, object->description,
			      object->organization);
			rc = SLURM_ERROR;
			continue;
		}
		xstrcat(cols, "creation_time, mod_time, name, "
			"description, organization");
		xstrfmtcat(vals, "%d, %d, '%s', '%s', '%s'", 
			   now, now, object->name, 
			   object->description, object->organization); 
		xstrfmtcat(extra, ", description='%s', organization='%s'",
			   object->description, object->organization); 		
		
		if(object->qos != ACCT_QOS_NOTSET) {
			xstrcat(cols, ", qos");
			xstrfmtcat(vals, ", %u", object->qos); 		
			xstrfmtcat(extra, ", qos=%u", object->qos); 		
		}

		query = xstrdup_printf(
			"insert into %s (%s) values (%s) "
			"on duplicate key update deleted=0, mod_time=%d %s;",
			acct_table, cols, vals,
			now, extra);
		debug3("%d query\n%s", mysql_conn->conn, query);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
		xfree(cols);
		xfree(vals);
		xfree(query);
		if(rc != SLURM_SUCCESS) {
			error("Couldn't add acct");
			xfree(extra);
			continue;
		}
		affect_rows = _last_affected_rows(mysql_conn->acct_mysql_db);
/* 		debug3("affected %d", affect_rows); */

		if(!affect_rows) {
			debug3("nothing changed");
			xfree(extra);
			continue;
		}

		if(txn_query)
			xstrfmtcat(txn_query, 	
				   ", (%d, %u, '%s', '%s', \"%s\")",
				   now, DBD_ADD_ACCOUNTS, object->name,
				   user, extra);
		else
			xstrfmtcat(txn_query, 	
				   "insert into %s "
				   "(timestamp, action, name, actor, info) "
				   "values (%d, %u, '%s', '%s', \"%s\")",
				   txn_table,
				   now, DBD_ADD_ACCOUNTS, object->name,
				   user, extra);
		xfree(extra);
		
		if(!object->assoc_list)
			continue;

		list_transfer(assoc_list, object->assoc_list);
	}
	list_iterator_destroy(itr);
	
	if(rc != SLURM_ERROR) {
		if(txn_query) {
			xstrcat(txn_query, ";");
			rc = mysql_db_query(mysql_conn->acct_mysql_db,
					    txn_query);
			xfree(txn_query);
			if(rc != SLURM_SUCCESS) {
				error("Couldn't add txn");
				rc = SLURM_SUCCESS;
			}
		}
	} else
		xfree(txn_query);

	if(list_count(assoc_list)) {
		if(acct_storage_p_add_associations(mysql_conn, uid, assoc_list)
		   == SLURM_ERROR) {
			error("Problem adding user associations");
			rc = SLURM_ERROR;
		}
	}
	list_destroy(assoc_list);

	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int acct_storage_p_add_clusters(mysql_conn_t *mysql_conn, uint32_t uid, 
				       List cluster_list)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;
	acct_cluster_rec_t *object = NULL;
	char *cols = NULL, *vals = NULL, *extra = NULL, *query = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user = NULL;
	int affect_rows = 0;

	if((pw=getpwuid(uid))) {
		user = pw->pw_name;
	}

	itr = list_iterator_create(cluster_list);
	while((object = list_next(itr))) {
		if(!object->name) {
			error("We need a cluster name to add.");
			rc = SLURM_ERROR;
			continue;
		}

		xstrcat(cols, "creation_time, mod_time, acct, cluster");
		xstrfmtcat(vals, "%d, %d, 'root', '%s'",
			   now, now, object->name);
		xstrfmtcat(extra, ", mod_time=%d", now);
	
		if((int)object->default_fairshare >= 0) {
			xstrcat(cols, ", fairshare");
			xstrfmtcat(vals, ", %u", object->default_fairshare);
			xstrfmtcat(extra, ", fairshare=%u",
				   object->default_fairshare);
		} else if ((int)object->default_fairshare == -1) {
			xstrcat(cols, ", fairshare");
			xstrfmtcat(vals, ", NULL");
			xstrfmtcat(extra, ", fairshare=NULL");		
		}

		if((int)object->default_max_cpu_secs_per_job >= 0) {
			xstrcat(cols, ", max_cpu_secs_per_job");
			xstrfmtcat(vals, ", %u",
				   object->default_max_cpu_secs_per_job);
			xstrfmtcat(extra, ", max_cpu_secs_per_job=%u",
				   object->default_max_cpu_secs_per_job);
		} else if((int)object->default_max_cpu_secs_per_job == -1) {
			xstrcat(cols, ", max_cpu_secs_per_job");
			xstrfmtcat(vals, ", NULL");
			xstrfmtcat(extra, ", max_cpu_secs_per_job=NULL");
		}
		
		if((int)object->default_max_jobs >= 0) {
			xstrcat(cols, ", max_jobs");
			xstrfmtcat(vals, ", %u", object->default_max_jobs);
			xstrfmtcat(extra, ", max_jobs=%u",
				   object->default_max_jobs);
		} else if((int)object->default_max_jobs == -1) {
			xstrcat(cols, ", max_jobs");
			xstrfmtcat(vals, ", NULL");
			xstrfmtcat(extra, ", max_jobs=NULL");		
		}

		if((int)object->default_max_nodes_per_job >= 0) {
			xstrcat(cols, ", max_nodes_per_job");
			xstrfmtcat(vals, ", %u", 
				   object->default_max_nodes_per_job);
			xstrfmtcat(extra, ", max_nodes_per_job=%u",
				   object->default_max_nodes_per_job);
		} else if((int)object->default_max_nodes_per_job == -1) {
			xstrcat(cols, ", max_nodes_per_job");
			xstrfmtcat(vals, ", NULL");
			xstrfmtcat(extra, ", max_nodes_per_job=NULL");
		}

		if((int)object->default_max_wall_duration_per_job >= 0) {
			xstrcat(cols, ", max_wall_duration_per_job");
			xstrfmtcat(vals, ", %u",
				   object->default_max_wall_duration_per_job);
			xstrfmtcat(extra, ", max_wall_duration_per_job=%u",
				   object->default_max_wall_duration_per_job);
		} else if((int)object->default_max_wall_duration_per_job
			  == -1) {
			xstrcat(cols, ", max_wall_duration_per_job");
			xstrfmtcat(vals, ", NULL");
			xstrfmtcat(extra, ", max_duration_per_job=NULL");
		}

		xstrfmtcat(query, 
			   "insert into %s (creation_time, mod_time, name) "
			   "values (%d, %d, '%s') "
			   "on duplicate key update deleted=0, mod_time=%d;",
			   cluster_table, 
			   now, now, object->name,
			   now);
		debug3("%d query\n%s", mysql_conn->conn, query);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
		xfree(query);
		if(rc != SLURM_SUCCESS) {
			error("Couldn't add cluster %s", object->name);
			xfree(extra);
			xfree(cols);
			xfree(vals);
			continue;
		}

		affect_rows = _last_affected_rows(mysql_conn->acct_mysql_db);

		if(!affect_rows) {
			debug2("nothing changed %d", affect_rows);
			xfree(extra);
			xfree(cols);
			xfree(vals);
			continue;
		}

		xstrfmtcat(query,
			   "SELECT @MyMax := coalesce(max(rgt), 0) FROM %s "
			   "FOR UPDATE;",
			   assoc_table);
		xstrfmtcat(query,
			   "insert into %s (%s, lft, rgt) "
			   "values (%s, @MyMax+1, @MyMax+2) "
			   "on duplicate key update deleted=0, "
			   "id=LAST_INSERT_ID(id)%s;",
			   assoc_table, cols,
			   vals,
			   extra);
		
		xfree(cols);
		xfree(vals);
		debug3("%d query\n%s", mysql_conn->conn, query);

		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
		xfree(query);

		if(rc != SLURM_SUCCESS) {
			error("Couldn't add cluster root assoc");
			xfree(extra);
			continue;
		}
		xstrfmtcat(query,
			   "insert into %s "
			   "(timestamp, action, name, actor, info) "
			   "values (%d, %u, '%s', '%s', \"%s\");",
			   txn_table,
			   now, DBD_ADD_CLUSTERS, object->name, user, extra);
		xfree(extra);			
		debug4("query\n%s",query);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
		xfree(query);
		if(rc != SLURM_SUCCESS) {
			error("Couldn't add txn");
		}
	}
	list_iterator_destroy(itr);

	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int acct_storage_p_add_associations(mysql_conn_t *mysql_conn,
					   uint32_t uid, 
					   List association_list)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	int rc = SLURM_SUCCESS;
	int i=0;
	acct_association_rec_t *object = NULL;
	char *cols = NULL, *vals = NULL, *txn_query = NULL,
		*extra = NULL, *query = NULL, *update = NULL;
	char *parent = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user = NULL;
	char *tmp_char = NULL;
	int assoc_id = 0;
	int incr = 0, my_left = 0;
	int affect_rows = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	char *old_parent = NULL, *old_cluster = NULL;
	char *massoc_req_inx[] = {
		"id",
		"parent_acct",
		"lft",
		"rgt",
		"deleted"
	};
	
	enum {
		MASSOC_ID,
		MASSOC_PACCT,
		MASSOC_LFT,
		MASSOC_RGT,
		MASSOC_DELETED,
		MASSOC_COUNT
	};

	if(!association_list) {
		error("No association list given");
		return SLURM_ERROR;
	}

	if((pw=getpwuid(uid))) {
		user = pw->pw_name;
	}

	itr = list_iterator_create(association_list);
	while((object = list_next(itr))) {
		if(!object->cluster || !object->acct) {
			error("We need a association cluster and "
			      "acct to add one.");
			rc = SLURM_ERROR;
			continue;
		}

		if(object->parent_acct) {
			parent = object->parent_acct;
		} else if(object->user) {
			parent = object->acct;
		} else {
			parent = "root";
		}

		xstrcat(cols, "creation_time, mod_time, cluster, acct");
		xstrfmtcat(vals, "%d, %d, '%s', '%s'", 
			   now, now, object->cluster, object->acct); 
		xstrfmtcat(update, "where id>=0 && cluster='%s' && acct='%s'",
			   object->cluster, object->acct); 

		xstrfmtcat(extra, ", mod_time=%d", now);
		if(!object->user) {
			xstrcat(cols, ", parent_acct");
			xstrfmtcat(vals, ", '%s'", parent);
			xstrfmtcat(extra, ", parent_acct='%s'", parent);
			xstrfmtcat(update, " && user=''"); 
		}
		
		if(object->user) {
			xstrcat(cols, ", user");
			xstrfmtcat(vals, ", '%s'", object->user); 		
			xstrfmtcat(extra, ", user='%s'", object->user);
			xstrfmtcat(update, " && user='%s'",
				   object->user); 
			
			if(object->partition) {
				xstrcat(cols, ", partition");
				xstrfmtcat(vals, ", '%s'", object->partition);
				xstrfmtcat(extra, ", partition='%s'",
					   object->partition);
				xstrfmtcat(update, " && partition='%s'",
					   object->partition);
			}
		}

		if((int)object->fairshare >= 0) {
			xstrcat(cols, ", fairshare");
			xstrfmtcat(vals, ", %d", object->fairshare);
			xstrfmtcat(extra, ", fairshare=%d",
				   object->fairshare);
		}

		if((int)object->max_jobs >= 0) {
			xstrcat(cols, ", max_jobs");
			xstrfmtcat(vals, ", %d", object->max_jobs);
			xstrfmtcat(extra, ", max_jobs=%d",
				   object->max_jobs);
		}

		if((int)object->max_nodes_per_job >= 0) {
			xstrcat(cols, ", max_nodes_per_job");
			xstrfmtcat(vals, ", %d", object->max_nodes_per_job);
			xstrfmtcat(extra, ", max_nodes_per_job=%d",
				   object->max_nodes_per_job);
		}

		if((int)object->max_wall_duration_per_job >= 0) {
			xstrcat(cols, ", max_wall_duration_per_job");
			xstrfmtcat(vals, ", %d",
				   object->max_wall_duration_per_job);
			xstrfmtcat(extra, ", max_wall_duration_per_job=%d",
				   object->max_wall_duration_per_job);
		}

		if((int)object->max_cpu_secs_per_job >= 0) {
			xstrcat(cols, ", max_cpu_secs_per_job");
			xstrfmtcat(vals, ", %d", object->max_cpu_secs_per_job);
			xstrfmtcat(extra, ", max_cpu_secs_per_job=%d",
				   object->max_cpu_secs_per_job);
		}

		for(i=0; i<MASSOC_COUNT; i++) {
			if(i) 
				xstrcat(tmp_char, ", ");
			xstrcat(tmp_char, massoc_req_inx[i]);
		}
		
		xstrfmtcat(query, 
			   "select distinct %s from %s %s  order by lft "
			   "FOR UPDATE;",
			   tmp_char, assoc_table, update);
		xfree(tmp_char);
		debug3("%d query\n%s", mysql_conn->conn, query);
		if(!(result = mysql_db_query_ret(
			     mysql_conn->acct_mysql_db, query, 0))) {
			xfree(query);
			xfree(cols);
			xfree(vals);
			xfree(extra);
			xfree(update);
			error("couldn't query the database");
			rc = SLURM_ERROR;
			break;
		}
		xfree(query);

		assoc_id = 0;
		if(!(row = mysql_fetch_row(result))) {
			if(!old_parent || !old_cluster
			   || strcasecmp(parent, old_parent) 
			   || strcasecmp(object->cluster, old_cluster)) {
				char *sel_query = xstrdup_printf(
					"SELECT lft FROM %s WHERE "
					"acct = '%s' and cluster = '%s' "
					"and user = '' order by lft;",
					assoc_table,
					parent, object->cluster);
				MYSQL_RES *sel_result = NULL;
				
				if(incr) {
					char *up_query = xstrdup_printf(
						"UPDATE %s SET rgt = rgt+%d "
						"WHERE rgt > %d && deleted < 2;"
						"UPDATE %s SET lft = lft+%d "
						"WHERE lft > %d "
						"&& deleted < 2;"
						"UPDATE %s SET deleted = 0 "
						"WHERE deleted = 2;",
						assoc_table, incr,
						my_left,
						assoc_table, incr,
						my_left,
						assoc_table);
					debug3("%d query\n%s", mysql_conn->conn,
					       up_query);
					rc = mysql_db_query(
						mysql_conn->acct_mysql_db,
						up_query);
					xfree(up_query);
					if(rc != SLURM_SUCCESS) {
						error("Couldn't do update");
						xfree(cols);
						xfree(vals);
						xfree(update);
						xfree(extra);
						xfree(sel_query);
						break;
					}
				}

				debug3("%d query\n%s", mysql_conn->conn,
				       sel_query);
				if(!(sel_result = mysql_db_query_ret(
					     mysql_conn->acct_mysql_db,
					     sel_query, 0))) {
					xfree(cols);
					xfree(vals);
					xfree(update);
					xfree(extra);
					xfree(sel_query);
					rc = SLURM_ERROR;
					break;
				}
				
				if(!(row = mysql_fetch_row(sel_result))) {
					error("Couldn't get left from query\n",
					      sel_query);
					mysql_free_result(sel_result);
					xfree(cols);
					xfree(vals);
					xfree(update);
					xfree(extra);
					xfree(sel_query);
					rc = SLURM_ERROR;
					break;
				}
				xfree(sel_query);

				my_left = atoi(row[0]);
				mysql_free_result(sel_result);
				//info("left is %d", my_left);
				xfree(old_parent);
				xfree(old_cluster);
				old_parent = xstrdup(parent);
				old_cluster = xstrdup(object->cluster);
				incr = 0;
			}
			incr += 2;
			xstrfmtcat(query,
				   "insert into %s (%s, lft, rgt, deleted) "
				   "values (%s, %d, %d, 2);",
				   assoc_table, cols,
				   vals, my_left+(incr-1), my_left+incr);
			
			/* definantly works but slow */
/* 			xstrfmtcat(query, */
/* 				   "SELECT @myLeft := lft FROM %s WHERE " */
/* 				   "acct = '%s' " */
/* 				   "and cluster = '%s' and user = '';", */
/* 				   assoc_table, */
/* 				   parent, */
/* 				   object->cluster); */
/* 			xstrfmtcat(query, */
/* 				   "UPDATE %s SET rgt = rgt+2 " */
/* 				   "WHERE rgt > @myLeft;" */
/* 				   "UPDATE %s SET lft = lft+2 " */
/* 				   "WHERE lft > @myLeft;", */
/* 				   assoc_table, */
/* 				   assoc_table); */
/* 			xstrfmtcat(query, */
/* 				   "insert into %s (%s, lft, rgt) " */
/* 				   "values (%s, @myLeft+1, @myLeft+2);", */
/* 				   assoc_table, cols, */
/* 				   vals); */
		} else if(!atoi(row[MASSOC_DELETED])) {
			debug("This account was added already");
			xfree(cols);
			xfree(vals);
			xfree(update);
			mysql_free_result(result);
			xfree(extra);
			continue;
		} else {
			assoc_id = atoi(row[MASSOC_ID]);
			if(object->parent_acct 
			   && strcasecmp(object->parent_acct,
					 row[MASSOC_PACCT])) {
				
				/* We need to move the parent! */
				if(_move_parent(mysql_conn,
						atoi(row[MASSOC_LFT]),
						atoi(row[MASSOC_RGT]),
						object->cluster,
						row[MASSOC_ID],
						row[MASSOC_PACCT],
						object->parent_acct)
				   == SLURM_ERROR)
					continue;
			}


			affect_rows = 2;
			xstrfmtcat(query,
				   "update %s set deleted=0, "
				   "id=LAST_INSERT_ID(id)%s %s;",
				   assoc_table, 
				   extra, update);
		}
		mysql_free_result(result);

		xfree(cols);
		xfree(vals);
		xfree(update);
		debug3("%d query\n%s", mysql_conn->conn, query);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
		xfree(query);
		if(rc != SLURM_SUCCESS) {
			error("Couldn't add assoc");
			xfree(extra);
			break;
		}
		/* see if this was an insert or update.  On an update
		 * the assoc_id will already be set
		 */
		if(!assoc_id) {
			affect_rows = _last_affected_rows(
				mysql_conn->acct_mysql_db);
			assoc_id = mysql_insert_id(mysql_conn->acct_mysql_db);
			//info("last id was %d", assoc_id);
		}

		object->id = assoc_id;

		if(_addto_update_list(mysql_conn->update_list, ACCT_ADD_ASSOC,
				      object) == SLURM_SUCCESS) {
			list_remove(itr);
		}

		if(txn_query)
			xstrfmtcat(txn_query, 	
				   ", (%d, %d, '%d', '%s', \"%s\")",
				   now, DBD_ADD_ASSOCS, assoc_id, user, extra);
		else
			xstrfmtcat(txn_query, 	
				   "insert into %s "
				   "(timestamp, action, name, actor, info) "
				   "values (%d, %d, '%d', '%s', \"%s\")",
				   txn_table,
				   now, DBD_ADD_ASSOCS, assoc_id, user, extra);
		xfree(extra);
	}
	list_iterator_destroy(itr);
	if(incr) {
		char *up_query = xstrdup_printf(
			"UPDATE %s SET rgt = rgt+%d "
			"WHERE rgt > %d && deleted < 2;"
			"UPDATE %s SET lft = lft+%d "
			"WHERE lft > %d "
			"&& deleted < 2;"
			"UPDATE %s SET deleted = 0 "
			"WHERE deleted = 2;",
			assoc_table, incr,
			my_left,
			assoc_table, incr,
			my_left,
			assoc_table);
		debug3("%d query\n%s", mysql_conn->conn, up_query);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, up_query);
		xfree(up_query);
		if(rc != SLURM_SUCCESS) 
			error("Couldn't do update 2");
		
	}

	if(rc != SLURM_ERROR) {
		if(txn_query) {
			xstrcat(txn_query, ";");
			rc = mysql_db_query(mysql_conn->acct_mysql_db,
					    txn_query);
			xfree(txn_query);
			if(rc != SLURM_SUCCESS) {
				error("Couldn't add txn");
				rc = SLURM_SUCCESS;
			}
		}
	} else
		xfree(txn_query);
	
	xfree(old_parent);
	xfree(old_cluster);
					
	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern List acct_storage_p_modify_users(mysql_conn_t *mysql_conn, uint32_t uid, 
					acct_user_cond_t *user_q,
					acct_user_rec_t *user)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	List ret_list = NULL;
	int rc = SLURM_SUCCESS;
	char *object = NULL;
	char *vals = NULL, *extra = NULL, *query = NULL, *name_char = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user_name = NULL;
	int set = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	if(!user_q) {
		error("we need something to change");
		return NULL;
	}

	if((pw=getpwuid(uid))) {
		user_name = pw->pw_name;
	}

	xstrcat(extra, "where deleted=0");
	if(user_q->user_list && list_count(user_q->user_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(user_q->user_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(user_q->def_acct_list && list_count(user_q->def_acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(user_q->def_acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "default_acct='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(user_q->qos != ACCT_QOS_NOTSET) {
		xstrfmtcat(extra, " && qos=%u", user_q->qos);
	}

	if(user_q->admin_level != ACCT_ADMIN_NOTSET) {
		xstrfmtcat(extra, " && admin_level=%u", user_q->admin_level);
	}

	if(user->default_acct)
		xstrfmtcat(vals, ", default_acct='%s'", user->default_acct);

	if(user->qos != ACCT_QOS_NOTSET)
		xstrfmtcat(vals, ", qos=%u", user->qos);

	if(user->admin_level != ACCT_ADMIN_NOTSET)
		xstrfmtcat(vals, ", admin_level=%u", user->admin_level);

	if(!extra || !vals) {
		error("Nothing to change");
		return NULL;
	}
	query = xstrdup_printf("select name from %s %s;", user_table, extra);
	xfree(extra);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	rc = 0;
	ret_list = list_create(slurm_destroy_char);
	while((row = mysql_fetch_row(result))) {
		object = xstrdup(row[0]);
		list_append(ret_list, object);
		if(!rc) {
			xstrfmtcat(name_char, "(name='%s'", object);
			rc = 1;
		} else  {
			xstrfmtcat(name_char, " || name='%s'", object);
		}
	}
	mysql_free_result(result);

	if(!list_count(ret_list)) {
		debug3("didn't effect anything");
		xfree(vals);
		return ret_list;
	}
	xstrcat(name_char, ")");

	if(_modify_common(mysql_conn, DBD_MODIFY_USERS, now,
			  user_name, user_table, name_char, vals)
	   == SLURM_ERROR) {
		error("Couldn't modify users");
		list_destroy(ret_list);
		ret_list = NULL;
	}

	xfree(name_char);
	xfree(vals);
				
	return ret_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_modify_accts(mysql_conn_t *mysql_conn, uint32_t uid, 
					acct_account_cond_t *acct_q,
					acct_account_rec_t *acct)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	List ret_list = NULL;
	int rc = SLURM_SUCCESS;
	char *object = NULL;
	char *vals = NULL, *extra = NULL, *query = NULL, *name_char = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user = NULL;
	int set = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	if(!acct_q) {
		error("we need something to change");
		return NULL;
	}

	if((pw=getpwuid(uid))) {
		user = pw->pw_name;
	}

	xstrcat(extra, "where deleted=0");
	if(acct_q->acct_list && list_count(acct_q->acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(acct_q->description_list && list_count(acct_q->description_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->description_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "description='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(acct_q->organization_list && list_count(acct_q->organization_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->organization_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "organization='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(acct_q->qos != ACCT_QOS_NOTSET) {
		xstrfmtcat(extra, " && qos=%u", acct_q->qos);
	}

	if(acct->description)
		xstrfmtcat(vals, ", description='%s'", acct->description);
	if(acct->organization)
		xstrfmtcat(vals, ", organization='%u'", acct->organization);
	if(acct->qos != ACCT_QOS_NOTSET)
		xstrfmtcat(vals, ", qos='%u'", acct->qos);

	if(!extra || !vals) {
		error("Nothing to change");
		return NULL;
	}

	query = xstrdup_printf("select name from %s %s;", acct_table, extra);
	xfree(extra);
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	rc = 0;
	ret_list = list_create(slurm_destroy_char);
	while((row = mysql_fetch_row(result))) {
		object = xstrdup(row[0]);
		list_append(ret_list, object);
		if(!rc) {
			xstrfmtcat(name_char, "(name='%s'", object);
			rc = 1;
		} else  {
			xstrfmtcat(name_char, " || name='%s'", object);
		}

	}
	mysql_free_result(result);

	if(!list_count(ret_list)) {
		debug3("didn't effect anything");
		xfree(vals);
		return ret_list;
	}
	xstrcat(name_char, ")");

	if(_modify_common(mysql_conn, DBD_MODIFY_ACCOUNTS, now,
			  user, acct_table, name_char, vals)
	   == SLURM_ERROR) {
		error("Couldn't modify accounts");
		list_destroy(ret_list);
		ret_list = NULL;
	}
		
	xfree(name_char);
	xfree(vals);

	return ret_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_modify_clusters(mysql_conn_t *mysql_conn, 
					   uint32_t uid, 
					   acct_cluster_cond_t *cluster_q,
					   acct_cluster_rec_t *cluster)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	List ret_list = NULL;
	int rc = SLURM_SUCCESS;
	char *object = NULL;
	char *vals = NULL, *assoc_vals = NULL, *extra = NULL, *query = NULL,
		*name_char = NULL, *assoc_char= NULL, *send_char = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user = NULL;
	int set = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	if(!cluster_q) {
		error("we need something to change");
		return NULL;
	}

	if((pw=getpwuid(uid))) {
		user = pw->pw_name;
	}

	xstrcat(extra, "where deleted=0");
	if(cluster_q->cluster_list && list_count(cluster_q->cluster_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(cluster_q->cluster_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

		
	if(cluster->control_host) {
		xstrfmtcat(vals, ", control_host='%s'", cluster->control_host);
	}
	if(cluster->control_port) {
		xstrfmtcat(vals, ", control_port=%u", cluster->control_port);
	}

	if((int)cluster->default_fairshare >= 0) {
		xstrfmtcat(assoc_vals, ", fairshare=%u",
			   cluster->default_fairshare);
	} else if((int)cluster->default_fairshare == -1) 
		xstrfmtcat(assoc_vals, ", fairshare=1");

	if((int)cluster->default_max_cpu_secs_per_job >= 0) {
		xstrfmtcat(assoc_vals, ", max_cpu_secs_per_job=%u",
			   cluster->default_max_cpu_secs_per_job);
	} else if((int)cluster->default_max_cpu_secs_per_job == -1) 
		xstrfmtcat(assoc_vals, ", max_cpu_secs_per_job=NULL");

	if((int)cluster->default_max_jobs >= 0) {
		xstrfmtcat(assoc_vals, ", max_jobs=%u",
			   cluster->default_max_jobs);
	} else if((int)cluster->default_max_jobs == -1)
		xstrfmtcat(assoc_vals, ", max_jobs=NULL");
	

	if((int)cluster->default_max_nodes_per_job >= 0) {
		xstrfmtcat(assoc_vals, ", max_nodes_per_job=%u",
			   cluster->default_max_nodes_per_job);
	} else if((int)cluster->default_max_nodes_per_job == -1)
		xstrfmtcat(assoc_vals, ", max_nodes_per_job=NULL");


	if((int)cluster->default_max_wall_duration_per_job >= 0) {
		xstrfmtcat(assoc_vals, ", max_wall_duration_per_job=%u",
			   cluster->default_max_wall_duration_per_job);
	} else if((int)cluster->default_max_wall_duration_per_job == -1) 
		xstrfmtcat(assoc_vals, ", max_wall_duration_per_job=NULL");
	

	if(!vals && !assoc_vals) {
		error("Nothing to change");
		return NULL;
	}

	xstrfmtcat(query, "select name from %s %s;", cluster_table, extra);
	xfree(extra);
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		xfree(vals);
		xfree(assoc_vals);
		error("no result given for %s", extra);
		return NULL;
	}
	xfree(query);
	
	rc = 0;
	ret_list = list_create(slurm_destroy_char);
	while((row = mysql_fetch_row(result))) {
		acct_association_rec_t *assoc = NULL;

		object = xstrdup(row[0]);
		list_append(ret_list, object);
		if(!rc) {
			xstrfmtcat(name_char, "name='%s'", object);
			xstrfmtcat(assoc_char, "cluster='%s'", object);
			rc = 1;
		} else  {
			xstrfmtcat(name_char, " || name='%s'", object);
			xstrfmtcat(assoc_char, " || cluster='%s'", object);
		}
		if(assoc_vals) {
			assoc = xmalloc(sizeof(acct_association_rec_t));
			assoc->cluster = xstrdup(object);
			assoc->acct = xstrdup("root");
			assoc->fairshare = cluster->default_fairshare;
			assoc->max_jobs = cluster->default_max_jobs;
			assoc->max_nodes_per_job =
				cluster->default_max_nodes_per_job;
			assoc->max_wall_duration_per_job = 
				cluster->default_max_wall_duration_per_job;
			assoc->max_cpu_secs_per_job = 
				cluster->default_max_cpu_secs_per_job;
			
			if(_addto_update_list(mysql_conn->update_list, 
					      ACCT_MODIFY_ASSOC,
					      assoc) != SLURM_SUCCESS) 
				error("couldn't add to the update list");
		}
	}
	mysql_free_result(result);

	if(!list_count(ret_list)) {
		debug3("didn't effect anything");
		xfree(assoc_vals);
		xfree(vals);
		return ret_list;
	}

	if(vals) {
		send_char = xstrdup_printf("(%s)", name_char);
		if(_modify_common(mysql_conn, DBD_MODIFY_CLUSTERS, now,
				  user, cluster_table, send_char, vals)
		   == SLURM_ERROR) {
			error("Couldn't modify cluster 1");
			list_destroy(ret_list);
			ret_list = NULL;
			goto end_it;
		}
	}

	if(assoc_vals) {
		send_char = xstrdup_printf("acct='root' && (%s)",
					   assoc_char);
		if(_modify_common(mysql_conn, DBD_MODIFY_CLUSTERS, now,
				  user, assoc_table, send_char, assoc_vals)
		   == SLURM_ERROR) {
			error("Couldn't modify cluster");
			list_destroy(ret_list);
			ret_list = NULL;
			goto end_it;
		}
	}
end_it:
	xfree(name_char);
	xfree(assoc_char);
	xfree(vals);
	xfree(assoc_vals);
	xfree(send_char);

	return ret_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_modify_associations(mysql_conn_t *mysql_conn, 
					       uint32_t uid, 
					       acct_association_cond_t *assoc_q,
					       acct_association_rec_t *assoc)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	List ret_list = NULL;
	int rc = SLURM_SUCCESS;
	char *object = NULL;
	char *vals = NULL, *extra = NULL, *query = NULL, *name_char = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user = NULL;
	int set = 0, i = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	char *massoc_req_inx[] = {
		"id",
		"acct",
		"parent_acct",
		"cluster",
		"user",
		"partition",
		"lft",
		"rgt"
	};
	
	enum {
		MASSOC_ID,
		MASSOC_ACCT,
		MASSOC_PACCT,
		MASSOC_CLUSTER,
		MASSOC_USER,
		MASSOC_PART,
		MASSOC_LFT,
		MASSOC_RGT,
		MASSOC_COUNT
	};

	if(!assoc_q) {
		error("we need something to change");
		return NULL;
	}

	if((pw=getpwuid(uid))) {
		user = pw->pw_name;
	}

	if(assoc_q->acct_list && list_count(assoc_q->acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "acct='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(assoc_q->cluster_list && list_count(assoc_q->cluster_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->cluster_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "cluster='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(assoc_q->user_list && list_count(assoc_q->user_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->user_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "user='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	} else {
		info("no user specified");
		xstrcat(extra, " && user = '' ");
	}

	if(assoc_q->id_list && list_count(assoc_q->id_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->id_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "id=%s", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(assoc_q->parent_acct) {
		xstrfmtcat(extra, " && parent_acct='%s'", assoc_q->parent_acct);
	}

	if((int)assoc->fairshare >= 0) 
		xstrfmtcat(vals, ", fairshare=%u", assoc->fairshare);
	else if((int)assoc->fairshare == -1) 
		xstrfmtcat(vals, ", fairshare=1");
       		
	if((int)assoc->max_cpu_secs_per_job >= 0) 
		xstrfmtcat(vals, ", max_cpu_secs_per_job=%u",
			   assoc->max_cpu_secs_per_job);
	else if((int)assoc->max_cpu_secs_per_job == -1) 
		xstrfmtcat(vals, ", max_cpu_secs_per_job=NULL");

	if((int)assoc->max_jobs >= 0) 
		xstrfmtcat(vals, ", max_jobs=%u", assoc->max_jobs);
	else if((int)assoc->max_jobs == -1)
		xstrfmtcat(vals, ", max_jobs=NULL");
		
	if((int)assoc->max_nodes_per_job >= 0) 
		xstrfmtcat(vals, ", max_nodes_per_job=%u",
			   assoc->max_nodes_per_job);
	else if((int)assoc->max_nodes_per_job == -1)
		xstrfmtcat(vals, ", max_nodes_per_job=NULL");

	if((int)assoc->max_wall_duration_per_job >= 0) 
		xstrfmtcat(vals, ", max_wall_duration_per_job=%u",
			   assoc->max_wall_duration_per_job);
	else if((int)assoc->max_wall_duration_per_job == -1) 
		xstrfmtcat(vals, ", max_wall_duration_per_job=NULL");
		
	if(!extra || (!vals && !assoc->parent_acct)) {
		error("Nothing to change");
		return NULL;
	}

	for(i=0; i<MASSOC_COUNT; i++) {
		if(i) 
			xstrcat(object, ", ");
		xstrcat(object, massoc_req_inx[i]);
	}

	query = xstrdup_printf("select distinct %s from %s where deleted=0%s "
			       "order by lft FOR UPDATE;",
			       object, assoc_table, extra);
	xfree(object);
	xfree(extra);

	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	rc = SLURM_SUCCESS;
	set = 0;
	extra = NULL;
	ret_list = list_create(slurm_destroy_char);
	while((row = mysql_fetch_row(result))) {
		acct_association_rec_t *mod_assoc = NULL;
		int account_type=0;
/* 		MYSQL_RES *result2 = NULL; */
/* 		MYSQL_ROW row2; */
		if(row[MASSOC_PART][0]) { 
			// see if there is a partition name
			object = xstrdup_printf(
				"C = %-10s A = %-20s U = %-9s P = %s",
				row[MASSOC_CLUSTER], row[MASSOC_ACCT],
				row[MASSOC_USER], row[MASSOC_PART]);
		} else if(row[MASSOC_USER][0]){
			object = xstrdup_printf(
				"C = %-10s A = %-20s U = %-9s",
				row[MASSOC_CLUSTER], row[MASSOC_ACCT], 
				row[MASSOC_USER]);
		} else {
			if(row[MASSOC_PACCT][0]) {
				object = xstrdup_printf(
					"C = %-10s A = %s of %s",
					row[MASSOC_CLUSTER], row[MASSOC_ACCT],
					row[MASSOC_PACCT]);
			} else {
				object = xstrdup_printf(
					"C = %-10s A = %s",
					row[MASSOC_CLUSTER], row[MASSOC_ACCT]);
			}
			if(assoc->parent_acct) {
				if(!strcasecmp(row[MASSOC_ACCT],
					       assoc->parent_acct)) {
					error("You can't make an account be a "
					      "child of it's self");
					xfree(object);
					continue;
				}

				if(_move_parent(mysql_conn,
						atoi(row[MASSOC_LFT]),
						atoi(row[MASSOC_RGT]),
						row[MASSOC_CLUSTER],
						row[MASSOC_ID],
						row[MASSOC_PACCT],
						assoc->parent_acct)
				   == SLURM_ERROR)
					break;
			}
			account_type = 1;
		}
		list_append(ret_list, object);

		if(!set) {
			xstrfmtcat(name_char, "(id=%s", row[MASSOC_ID]);
			set = 1;
		} else {
			xstrfmtcat(name_char, " || id=%s", row[MASSOC_ID]);
		}
		
		mod_assoc = xmalloc(sizeof(acct_association_rec_t));
		mod_assoc->id = atoi(row[MASSOC_ID]);

		mod_assoc->max_cpu_secs_per_job = assoc->max_cpu_secs_per_job;
		mod_assoc->fairshare = assoc->fairshare;
		mod_assoc->max_jobs = assoc->max_jobs;
		mod_assoc->max_nodes_per_job = assoc->max_nodes_per_job;
		mod_assoc->max_wall_duration_per_job = 
			assoc->max_wall_duration_per_job;
		if(!row[MASSOC_USER][0])
			mod_assoc->parent_acct = xstrdup(assoc->parent_acct);

		if(_addto_update_list(mysql_conn->update_list, 
				      ACCT_MODIFY_ASSOC,
				      mod_assoc) != SLURM_SUCCESS) 
			error("couldn't add to the update list");
		if(account_type) {
			_modify_unset_users(mysql_conn,
					    mod_assoc,
					    row[MASSOC_ACCT],
					    atoi(row[MASSOC_LFT]),
					    atoi(row[MASSOC_RGT]),
					    ret_list);
		}
	}
	mysql_free_result(result);

	if(assoc->parent_acct) {
		if(rc != SLURM_SUCCESS) {
			if(mysql_conn->rollback) {
				mysql_db_rollback(mysql_conn->acct_mysql_db);
			}
			list_destroy(mysql_conn->update_list);
			mysql_conn->update_list =
				list_create(destroy_acct_update_object);
			list_destroy(ret_list);
			xfree(vals);
			return NULL;
		}
	}


	if(!list_count(ret_list)) {
		debug3("didn't effect anything");
		xfree(vals);
		return ret_list;
	}
	xstrcat(name_char, ")");

	if(vals) {
		if(_modify_common(mysql_conn, DBD_MODIFY_ASSOCS, now,
				  user, assoc_table, name_char, vals)
		   == SLURM_ERROR) {
			error("Couldn't modify associations");
			list_destroy(ret_list);
			ret_list = NULL;
			goto end_it;
		}
	}

end_it:
	xfree(name_char);
	xfree(vals);

	return ret_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_remove_users(mysql_conn_t *mysql_conn, uint32_t uid, 
					acct_user_cond_t *user_q)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	List ret_list = NULL;
	int rc = SLURM_SUCCESS;
	char *object = NULL;
	char *extra = NULL, *query = NULL,
		*name_char = NULL, *assoc_char = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user_name = NULL;
	int set = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	if(!user_q) {
		error("we need something to change");
		return NULL;
	}

	if((pw=getpwuid(uid))) {
		user_name = pw->pw_name;
	}

	xstrcat(extra, "where deleted=0");

	if(user_q->user_list && list_count(user_q->user_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(user_q->user_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(user_q->def_acct_list && list_count(user_q->def_acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(user_q->def_acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "default_acct='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(user_q->qos != ACCT_QOS_NOTSET) {
		xstrfmtcat(extra, " && qos=%u", user_q->qos);
	}

	if(user_q->admin_level != ACCT_ADMIN_NOTSET) {
		xstrfmtcat(extra, " && admin_level=%u", user_q->admin_level);
	}

	if(!extra) {
		error("Nothing to remove");
		return NULL;
	}

	query = xstrdup_printf("select name from %s %s;", user_table, extra);
	xfree(extra);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	rc = 0;
	ret_list = list_create(slurm_destroy_char);
	while((row = mysql_fetch_row(result))) {
		char *object = xstrdup(row[0]);
		list_append(ret_list, object);
		if(!rc) {
			xstrfmtcat(name_char, "name='%s'", object);
			xstrfmtcat(assoc_char, "t2.user='%s'", object);
			rc = 1;
		} else {
			xstrfmtcat(name_char, " || name='%s'", object);
			xstrfmtcat(assoc_char, " || t2.user='%s'", object);
		}
	}
	mysql_free_result(result);

	if(!list_count(ret_list)) {
		debug3("didn't effect anything");
		return ret_list;
	}
	
	if(_remove_common(mysql_conn, DBD_REMOVE_USERS, now,
			  user_name, user_table, name_char, assoc_char)
	   == SLURM_ERROR) {
		list_destroy(ret_list);
		xfree(name_char);
		xfree(assoc_char);
		return NULL;
	}
	xfree(name_char);
	xfree(assoc_char);

	return ret_list;

#else
	return NULL;
#endif
}

extern List acct_storage_p_remove_coord(mysql_conn_t *mysql_conn, uint32_t uid, 
					char *acct, acct_user_cond_t *user_q)
{
#ifdef HAVE_MYSQL
	return SLURM_SUCCESS;
#else
	return NULL;
#endif
}

extern List acct_storage_p_remove_accts(mysql_conn_t *mysql_conn, uint32_t uid, 
					acct_account_cond_t *acct_q)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	List ret_list = NULL;
	int rc = SLURM_SUCCESS;
	char *object = NULL;
	char *extra = NULL, *query = NULL,
		*name_char = NULL, *assoc_char = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user_name = NULL;
	int set = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	if(!acct_q) {
		error("we need something to change");
		return NULL;
	}

	if((pw=getpwuid(uid))) {
		user_name = pw->pw_name;
	}

	xstrcat(extra, "where deleted=0");
	if(acct_q->acct_list && list_count(acct_q->acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(acct_q->description_list && list_count(acct_q->description_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->description_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "description='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(acct_q->organization_list && list_count(acct_q->organization_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->organization_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "organization='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(acct_q->qos != ACCT_QOS_NOTSET) {
		xstrfmtcat(extra, " && qos=%u", acct_q->qos);
	}

	if(!extra) {
		error("Nothing to remove");
		return NULL;
	}

	query = xstrdup_printf("select name from %s %s;", acct_table, extra);
	xfree(extra);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	rc = 0;
	ret_list = list_create(slurm_destroy_char);
	while((row = mysql_fetch_row(result))) {
		char *object = xstrdup(row[0]);
		list_append(ret_list, object);
		if(!rc) {
			xstrfmtcat(name_char, "name='%s'", object);
			xstrfmtcat(assoc_char, "t2.acct='%s'", object);
			rc = 1;
		} else  {
			xstrfmtcat(name_char, " || name='%s'", object);
			xstrfmtcat(assoc_char, " || t2.acct='%s'", object);
		}
	}
	mysql_free_result(result);

	if(!list_count(ret_list)) {
		debug3("didn't effect anything");
		return ret_list;
	}

	if(_remove_common(mysql_conn, DBD_REMOVE_ACCOUNTS, now,
			  user_name, acct_table, name_char, assoc_char)
	   == SLURM_ERROR) {
		list_destroy(ret_list);
		xfree(name_char);
		xfree(assoc_char);
		return NULL;
	}
	xfree(name_char);
	xfree(assoc_char);

	return ret_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_remove_clusters(mysql_conn_t *mysql_conn,
					   uint32_t uid, 
					   acct_cluster_cond_t *cluster_q)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	List ret_list = NULL;
	int rc = SLURM_SUCCESS;
	char *object = NULL;
	char *extra = NULL, *query = NULL,
		*name_char = NULL, *assoc_char = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user_name = NULL;
	int set = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	int day_old = now - DELETE_SEC_BACK;

	if(!cluster_q) {
		error("we need something to change");
		return NULL;
	}

	if((pw=getpwuid(uid))) {
		user_name = pw->pw_name;
	}
	xstrcat(extra, "where deleted=0");
	if(cluster_q->cluster_list && list_count(cluster_q->cluster_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(cluster_q->cluster_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(!extra) {
		error("Nothing to remove");
		return NULL;
	}

	query = xstrdup_printf("select name from %s %s;", cluster_table, extra);
	xfree(extra);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	rc = 0;
	ret_list = list_create(slurm_destroy_char);
	while((row = mysql_fetch_row(result))) {
		char *object = xstrdup(row[0]);
		list_append(ret_list, object);
		if(!rc) {
			xstrfmtcat(name_char, "name='%s'", object);
			xstrfmtcat(extra, "t2.cluster='%s'", object);
			xstrfmtcat(assoc_char, "cluster='%s'", object);
			rc = 1;
		} else  {
			xstrfmtcat(name_char, " || name='%s'", object);
			xstrfmtcat(extra, " || t2.cluster='%s'", object);
			xstrfmtcat(assoc_char, " || cluster='%s'", object);
		}
	}
	mysql_free_result(result);

	if(!list_count(ret_list)) {
		debug3("didn't effect anything\n%s", query);
		xfree(query);
		return ret_list;
	}
	xfree(query);

	/* if this is a cluster update the machine usage tables as well */
	query = xstrdup_printf("delete from %s where creation_time>%d && (%s);"
			       "delete from %s where creation_time>%d && (%s);"
			       "delete from %s where creation_time>%d && (%s);",
			       cluster_day_table, day_old, assoc_char,
			       cluster_hour_table, day_old, assoc_char,
			       cluster_month_table, day_old, assoc_char);
	xstrfmtcat(query,
		   "update %s set mod_time=%d, deleted=1 where (%s);"
		   "update %s set mod_time=%d, deleted=1 where (%s);"
		   "update %s set mod_time=%d, deleted=1 where (%s);",
		   cluster_day_table, now, assoc_char,
		   cluster_hour_table, now, assoc_char,
		   cluster_month_table, now, assoc_char);
	xfree(assoc_char);
	debug3("%d query\n%s", mysql_conn->conn, query);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);
	if(rc != SLURM_SUCCESS) {
		if(mysql_conn->rollback) {
			mysql_db_rollback(mysql_conn->acct_mysql_db);
		}
		list_flush(mysql_conn->update_list);
		list_destroy(ret_list);
		xfree(name_char);
		xfree(extra);
		return NULL;
	}

	assoc_char = xstrdup_printf("t2.acct='root' && (%s)", extra);
	xfree(extra);

	if(_remove_common(mysql_conn, DBD_REMOVE_CLUSTERS, now,
			  user_name, cluster_table, name_char, assoc_char)
	   == SLURM_ERROR) {
		list_destroy(ret_list);
		xfree(name_char);
		xfree(assoc_char);
		return NULL;
	}
	xfree(name_char);
	xfree(assoc_char);

	return ret_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_remove_associations(mysql_conn_t *mysql_conn,
					       uint32_t uid, 
					       acct_association_cond_t *assoc_q)
{
#ifdef HAVE_MYSQL
	ListIterator itr = NULL;
	List ret_list = NULL;
	int rc = SLURM_SUCCESS;
	char *object = NULL;
	char *extra = NULL, *query = NULL,
		*name_char = NULL, *assoc_char = NULL;
	time_t now = time(NULL);
	struct passwd *pw = NULL;
	char *user_name = NULL;
	int set = 0, i = 0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	/* if this changes you will need to edit the corresponding 
	 * enum below also t1 is step_table */
	char *rassoc_req_inx[] = {
		"id",
		"acct",
		"parent_acct",
		"cluster",
		"user",
		"partition"
	};
	
	enum {
		RASSOC_ID,
		RASSOC_ACCT,
		RASSOC_PACCT,
		RASSOC_CLUSTER,
		RASSOC_USER,
		RASSOC_PART,
		RASSOC_COUNT
	};

	if(!assoc_q) {
		error("we need something to change");
		return NULL;
	}

	xstrcat(extra, "where id>0 && deleted=0");

	if((pw=getpwuid(uid))) {
		user_name = pw->pw_name;
	}

	if(assoc_q->acct_list && list_count(assoc_q->acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "acct='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(assoc_q->cluster_list && list_count(assoc_q->cluster_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->cluster_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "cluster='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(assoc_q->user_list && list_count(assoc_q->user_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->user_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "user='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(assoc_q->id_list && list_count(assoc_q->id_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->id_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "id=%s", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(assoc_q->parent_acct) {
		xstrfmtcat(extra, " && parent_acct='%s'",
			   assoc_q->parent_acct);
	}

	for(i=0; i<RASSOC_COUNT; i++) {
		if(i) 
			xstrcat(object, ", ");
		xstrcat(object, rassoc_req_inx[i]);
	}

	query = xstrdup_printf("select lft, rgt from %s %s order by lft "
			       "FOR UPDATE;",
			       assoc_table, extra);
	xfree(extra);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);
		
	rc = 0;
	while((row = mysql_fetch_row(result))) {
		if(!rc) {
			xstrfmtcat(name_char, "lft between %s and %s",
				   row[0], row[1]);
			rc = 1;
		} else {
			xstrfmtcat(name_char, " || lft between %s and %s",
				   row[0], row[1]);
		}
	}
	mysql_free_result(result);

	if(!name_char) {
		debug3("didn't effect anything\n%s", query);
		xfree(query);
		return ret_list;
	}

	xfree(query);
	query = xstrdup_printf("select distinct %s "
			       "from %s where (%s) order by lft;",
			       object,
			       assoc_table, name_char);
	xfree(extra);
	xfree(object);
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		xfree(name_char);
		return NULL;
	}

	rc = 0;
	ret_list = list_create(slurm_destroy_char);
	while((row = mysql_fetch_row(result))) {
		acct_association_rec_t *rem_assoc = NULL;

		if(row[RASSOC_PART][0]) { 
			// see if there is a partition name
			object = xstrdup_printf(
				"C = %-10s A = %-10s U = %-9s P = %s",
				row[RASSOC_CLUSTER], row[RASSOC_ACCT],
				row[RASSOC_USER], row[RASSOC_PART]);
		} else if(row[RASSOC_USER][0]){
			object = xstrdup_printf(
				"C = %-10s A = %-10s U = %-9s",
				row[RASSOC_CLUSTER], row[RASSOC_ACCT], 
				row[RASSOC_USER]);
		} else {
			if(row[RASSOC_PACCT][0]) {
				object = xstrdup_printf(
					"C = %-10s A = %s of %s",
					row[RASSOC_CLUSTER], row[RASSOC_ACCT],
					row[RASSOC_PACCT]);
			} else {
				object = xstrdup_printf(
					"C = %-10s A = %s",
					row[RASSOC_CLUSTER], row[RASSOC_ACCT]);
			}
		}
		list_append(ret_list, object);
		if(!rc) {
			xstrfmtcat(assoc_char, "id=%s", row[RASSOC_ID]);
			rc = 1;
		} else {
			xstrfmtcat(assoc_char, " || id=%s", row[RASSOC_ID]);
		}

		rem_assoc = xmalloc(sizeof(acct_association_rec_t));
		rem_assoc->id = atoi(row[RASSOC_ID]);
		if(_addto_update_list(mysql_conn->update_list, 
				      ACCT_REMOVE_ASSOC,
				      rem_assoc) != SLURM_SUCCESS) 
			error("couldn't add to the update list");

	}
	mysql_free_result(result);

	if(_remove_common(mysql_conn, DBD_REMOVE_ASSOCS, now,
			  user_name, assoc_table, name_char, assoc_char)
	   == SLURM_ERROR) {
		list_destroy(ret_list);
		xfree(name_char);
		xfree(assoc_char);
		return NULL;
	}
	xfree(name_char);
	xfree(assoc_char);

	return ret_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_get_users(mysql_conn_t *mysql_conn, 
				     acct_user_cond_t *user_q)
{
#ifdef HAVE_MYSQL
	char *query = NULL;	
	char *extra = NULL;	
	char *tmp = NULL;	
	List user_list = NULL;
	ListIterator itr = NULL;
	char *object = NULL;
	int set = 0;
	int i=0;
	MYSQL_RES *result = NULL, *coord_result = NULL;
	MYSQL_ROW row, coord_row;

	/* if this changes you will need to edit the corresponding enum */
	char *user_req_inx[] = {
		"name",
		"default_acct",
		"qos",
		"admin_level"
	};
	enum {
		USER_REQ_NAME,
		USER_REQ_DA,
		USER_REQ_EX,
		USER_REQ_AL,
		USER_REQ_COUNT
	};

	xstrcat(extra, "where deleted=0");

	if(!user_q) 
		goto empty;

	if(user_q->user_list && list_count(user_q->user_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(user_q->user_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(user_q->def_acct_list && list_count(user_q->def_acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(user_q->def_acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "default_acct='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(user_q->qos != ACCT_QOS_NOTSET) {
		if(extra)
			xstrfmtcat(extra, " && qos=%u", user_q->qos);
		else
			xstrfmtcat(extra, " where qos=%u",
				   user_q->qos);
			
	}

	if(user_q->admin_level != ACCT_ADMIN_NOTSET) {
		if(extra)
			xstrfmtcat(extra, " && admin_level=%u",
				   user_q->admin_level);
		else
			xstrfmtcat(extra, " where admin_level=%u",
				   user_q->admin_level);
	}
empty:

	xfree(tmp);
	xstrfmtcat(tmp, "%s", user_req_inx[i]);
	for(i=1; i<USER_REQ_COUNT; i++) {
		xstrfmtcat(tmp, ", %s", user_req_inx[i]);
	}

	query = xstrdup_printf("select %s from %s %s", tmp, user_table, extra);
	xfree(tmp);
	xfree(extra);
	
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	user_list = list_create(destroy_acct_user_rec);

	while((row = mysql_fetch_row(result))) {
		acct_user_rec_t *user = xmalloc(sizeof(acct_user_rec_t));
		struct passwd *passwd_ptr = NULL;
		list_append(user_list, user);

		user->name =  xstrdup(row[USER_REQ_NAME]);
		user->default_acct = xstrdup(row[USER_REQ_DA]);
		user->admin_level = atoi(row[USER_REQ_AL]);
		user->qos = atoi(row[USER_REQ_EX]);

		passwd_ptr = getpwnam(user->name);
		if(passwd_ptr) 
			user->uid = passwd_ptr->pw_uid;
		
		user->coord_accts = list_create(destroy_acct_coord_rec);
		query = xstrdup_printf("select acct from %s where user='%s' "
				       "&& deleted=0",
				       acct_coord_table, user->name);

		if(!(coord_result =
		     mysql_db_query_ret(mysql_conn->acct_mysql_db, query, 0))) {
			xfree(query);
			continue;
		}
		xfree(query);
		
		while((coord_row = mysql_fetch_row(coord_result))) {
			acct_coord_rec_t *coord =
				xmalloc(sizeof(acct_coord_rec_t));
			list_append(user->coord_accts, coord);
			coord->acct_name = xstrdup(coord_row[0]);
			coord->sub_acct = 0;
		}
		mysql_free_result(coord_result);
		/* FIX ME: ADD SUB projects here from assoc list lft
		 * rgt */
		
		if(user_q && user_q->with_assocs) {
			acct_association_cond_t *assoc_q = NULL;
			if(!user_q->assoc_cond) {
				user_q->assoc_cond = xmalloc(
					sizeof(acct_association_cond_t));
			}
			assoc_q = user_q->assoc_cond;
			if(assoc_q->user_list)
				list_destroy(assoc_q->user_list);

			assoc_q->user_list = list_create(NULL);
			list_append(assoc_q->user_list, user->name);
			user->assoc_list = acct_storage_p_get_associations(
				mysql_conn, assoc_q);
			list_destroy(assoc_q->user_list);
			assoc_q->user_list = NULL;
		}
	}
	mysql_free_result(result);

	return user_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_get_accts(mysql_conn_t *mysql_conn, 
				     acct_account_cond_t *acct_q)
{
#ifdef HAVE_MYSQL
	char *query = NULL;	
	char *extra = NULL;	
	char *tmp = NULL;	
	List acct_list = NULL;
	ListIterator itr = NULL;
	char *object = NULL;
	int set = 0;
	int i=0;
	MYSQL_RES *result = NULL, *coord_result = NULL;
	MYSQL_ROW row, coord_row;

	/* if this changes you will need to edit the corresponding enum */
	char *acct_req_inx[] = {
		"name",
		"description",
		"qos",
		"organization"
	};
	enum {
		ACCT_REQ_NAME,
		ACCT_REQ_DESC,
		ACCT_REQ_QOS,
		ACCT_REQ_ORG,
		ACCT_REQ_COUNT
	};

	xstrcat(extra, "where deleted=0");
	if(!acct_q) 
		goto empty;

	if(acct_q->acct_list && list_count(acct_q->acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(acct_q->description_list && list_count(acct_q->description_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->description_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "description='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(acct_q->organization_list && list_count(acct_q->organization_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(acct_q->organization_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "organization='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(acct_q->qos != ACCT_QOS_NOTSET) {
		if(extra)
			xstrfmtcat(extra, " && qos=%u", acct_q->qos);
		else
			xstrfmtcat(extra, " where qos=%u",
				   acct_q->qos);
	}

empty:

	xfree(tmp);
	xstrfmtcat(tmp, "%s", acct_req_inx[i]);
	for(i=1; i<ACCT_REQ_COUNT; i++) {
		xstrfmtcat(tmp, ", %s", acct_req_inx[i]);
	}

	query = xstrdup_printf("select %s from %s %s", tmp, acct_table, extra);
	xfree(tmp);
	xfree(extra);
	
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	acct_list = list_create(destroy_acct_account_rec);

	while((row = mysql_fetch_row(result))) {
		acct_account_rec_t *acct = xmalloc(sizeof(acct_account_rec_t));
		list_append(acct_list, acct);

		acct->name =  xstrdup(row[ACCT_REQ_NAME]);
		acct->description = xstrdup(row[ACCT_REQ_DESC]);
		acct->organization = xstrdup(row[ACCT_REQ_ORG]);
		acct->qos = atoi(row[ACCT_REQ_QOS]);

		acct->coordinators = list_create(slurm_destroy_char);
		query = xstrdup_printf("select user from %s where acct='%s' "
				       "&& deleted=0;",
				       acct_coord_table, acct->name);

		if(!(coord_result =
		     mysql_db_query_ret(mysql_conn->acct_mysql_db, query, 0))) {
			xfree(query);
			continue;
		}
		xfree(query);
		
		while((coord_row = mysql_fetch_row(coord_result))) {
			object = xstrdup(coord_row[0]);
			list_append(acct->coordinators, object);
		}
		mysql_free_result(coord_result);

		if(acct_q && acct_q->with_assocs) {
			acct_association_cond_t *assoc_q = NULL;
			if(!acct_q->assoc_cond) {
				acct_q->assoc_cond = xmalloc(
					sizeof(acct_association_cond_t));
			}
			assoc_q = acct_q->assoc_cond;
			if(assoc_q->acct_list)
				list_destroy(assoc_q->acct_list);

			assoc_q->acct_list = list_create(NULL);
			list_append(assoc_q->acct_list, acct->name);
			acct->assoc_list = acct_storage_p_get_associations(
				mysql_conn, assoc_q);
			list_destroy(assoc_q->acct_list);
			assoc_q->acct_list = NULL;
		}

	}
	mysql_free_result(result);

	return acct_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_get_clusters(mysql_conn_t *mysql_conn, 
					acct_cluster_cond_t *cluster_q)
{
#ifdef HAVE_MYSQL
	char *query = NULL;	
	char *extra = NULL;	
	char *tmp = NULL;	
	List cluster_list = NULL;
	ListIterator itr = NULL;
	char *object = NULL;
	int set = 0;
	int i=0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	/* if this changes you will need to edit the corresponding enum */
	char *cluster_req_inx[] = {
		"name",
		"control_host",
		"control_port"
	};
	enum {
		CLUSTER_REQ_NAME,
		CLUSTER_REQ_CH,
		CLUSTER_REQ_CP,
		CLUSTER_REQ_COUNT
	};

	xstrcat(extra, "where deleted=0");
		
	if(!cluster_q) 
		goto empty;

	if(cluster_q->cluster_list && list_count(cluster_q->cluster_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(cluster_q->cluster_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "name='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

empty:

	xfree(tmp);
	xstrfmtcat(tmp, "%s", cluster_req_inx[i]);
	for(i=1; i<CLUSTER_REQ_COUNT; i++) {
		xstrfmtcat(tmp, ", %s", cluster_req_inx[i]);
	}

	query = xstrdup_printf("select %s from %s %s", 
			       tmp, cluster_table, extra);
	xfree(tmp);
	xfree(extra);
	
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	cluster_list = list_create(destroy_acct_cluster_rec);

	while((row = mysql_fetch_row(result))) {
		acct_cluster_rec_t *cluster =
			xmalloc(sizeof(acct_cluster_rec_t));
		list_append(cluster_list, cluster);

		cluster->name =  xstrdup(row[CLUSTER_REQ_NAME]);
		cluster->control_host = xstrdup(row[CLUSTER_REQ_CH]);
		cluster->control_port = atoi(row[CLUSTER_REQ_CP]);
	}
	mysql_free_result(result);

	return cluster_list;
#else
	return NULL;
#endif
}

extern List acct_storage_p_get_associations(mysql_conn_t *mysql_conn, 
					    acct_association_cond_t *assoc_q)
{
#ifdef HAVE_MYSQL
	char *query = NULL;	
	char *extra = NULL;	
	char *tmp = NULL;	
	List assoc_list = NULL;
	ListIterator itr = NULL;
	char *object = NULL;
	int set = 0;
	int i=0;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	int parent_mj = -1;
	int parent_mnpj = -1;
	int parent_mwpj = -1;
	int parent_mcpj = -1;
	char *last_acct = NULL;
	char *last_acct_parent = NULL;
	char *last_cluster = NULL;
	uint32_t user_parent_id = 0;
	uint32_t acct_parent_id = 0;

	/* if this changes you will need to edit the corresponding enum */
	char *assoc_req_inx[] = {
		"id",
		"user",
		"acct",
		"cluster",
		"partition",
		"parent_acct",
		"fairshare",
		"max_jobs",
		"max_nodes_per_job",
		"max_wall_duration_per_job",
		"max_cpu_secs_per_job",
	};
	enum {
		ASSOC_REQ_ID,
		ASSOC_REQ_USER,
		ASSOC_REQ_ACCT,
		ASSOC_REQ_CLUSTER,
		ASSOC_REQ_PART,
		ASSOC_REQ_PARENT,
		ASSOC_REQ_FS,
		ASSOC_REQ_MJ,
		ASSOC_REQ_MNPJ,
		ASSOC_REQ_MWPJ,
		ASSOC_REQ_MCPJ,
		ASSOC_REQ_COUNT
	};
	enum {
		ASSOC2_REQ_PARENT_ID,
		ASSOC2_REQ_MJ,
		ASSOC2_REQ_MNPJ,
		ASSOC2_REQ_MWPJ,
		ASSOC2_REQ_MCPJ
	};

	xstrcat(extra, "where deleted=0");
	if(!assoc_q) 
		goto empty;

	if(assoc_q->acct_list && list_count(assoc_q->acct_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->acct_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "acct='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(assoc_q->cluster_list && list_count(assoc_q->cluster_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->cluster_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "cluster='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(assoc_q->user_list && list_count(assoc_q->user_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->user_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "user='%s'", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}

	if(assoc_q->id_list && list_count(assoc_q->id_list)) {
		set = 0;
		xstrcat(extra, " && (");
		itr = list_iterator_create(assoc_q->id_list);
		while((object = list_next(itr))) {
			if(set) 
				xstrcat(extra, " || ");
			xstrfmtcat(extra, "id=%s", object);
			set = 1;
		}
		list_iterator_destroy(itr);
		xstrcat(extra, ")");
	}
	
	if(assoc_q->parent_acct) {
		xstrfmtcat(extra, " && parent_acct='%s'", assoc_q->parent_acct);
	}
empty:
	xfree(tmp);
	xstrfmtcat(tmp, "%s", assoc_req_inx[i]);
	for(i=1; i<ASSOC_REQ_COUNT; i++) {
		xstrfmtcat(tmp, ", %s", assoc_req_inx[i]);
	}

	query = xstrdup_printf("select %s from %s %s order by lft;", 
			       tmp, assoc_table, extra);
	xfree(tmp);
	xfree(extra);
	debug3("%d query\n%s", mysql_conn->conn, query);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return NULL;
	}
	xfree(query);

	assoc_list = list_create(destroy_acct_association_rec);
	
	while((row = mysql_fetch_row(result))) {
		acct_association_rec_t *assoc =
			xmalloc(sizeof(acct_association_rec_t));
		MYSQL_RES *result2 = NULL;
		MYSQL_ROW row2;

		list_append(assoc_list, assoc);
		
		assoc->id =  atoi(row[ASSOC_REQ_ID]);
		
		if(row[ASSOC_REQ_USER][0])
			assoc->user = xstrdup(row[ASSOC_REQ_USER]);
		assoc->acct = xstrdup(row[ASSOC_REQ_ACCT]);
		assoc->cluster = xstrdup(row[ASSOC_REQ_CLUSTER]);
		
		if(row[ASSOC_REQ_PARENT][0]) {
			if(!last_acct_parent || !last_cluster 
			   || strcmp(row[ASSOC_REQ_PARENT], last_acct_parent)
			   || strcmp(row[ASSOC_REQ_CLUSTER], last_cluster)) {
			
			query = xstrdup_printf(
					"select id from %s where user='' "
					"and deleted = 0 and acct='%s' "
					"and cluster='%s';", 
					assoc_table, row[ASSOC_REQ_PARENT],
					row[ASSOC_REQ_CLUSTER]);
			
				if(!(result2 = mysql_db_query_ret(
					     mysql_conn->acct_mysql_db,
					     query, 1))) {
					xfree(query);
					break;
				}
				xfree(query);
				row2 = mysql_fetch_row(result2);
				last_acct_parent = row[ASSOC_REQ_PARENT];
				last_cluster = row[ASSOC_REQ_CLUSTER];
				acct_parent_id = atoi(row2[0]);	
				mysql_free_result(result2);
			}
			assoc->parent_acct = xstrdup(row[ASSOC_REQ_PARENT]);
			assoc->parent_id = acct_parent_id;
		} 

		if(row[ASSOC_REQ_PART][0])
			assoc->partition = xstrdup(row[ASSOC_REQ_PART]);
		if(row[ASSOC_REQ_FS])
			assoc->fairshare = atoi(row[ASSOC_REQ_FS]);
		else
			assoc->fairshare = 1;

		if(!last_acct || !last_cluster 
		   || strcmp(row[ASSOC_REQ_ACCT], last_acct)
		   || strcmp(row[ASSOC_REQ_CLUSTER], last_cluster)) {
			query = xstrdup_printf(
				"call get_parent_limits('%s', '%s', '%s');"
				"select @par_id, @mj, @mnpj, @mwpj, @mcpj;", 
				assoc_table, row[ASSOC_REQ_ACCT],
				row[ASSOC_REQ_CLUSTER]);
			
			if(!(result2 = mysql_db_query_ret(
				     mysql_conn->acct_mysql_db, query, 1))) {
				xfree(query);
				break;
			}
			xfree(query);
			
			row2 = mysql_fetch_row(result2);
			user_parent_id = atoi(row2[ASSOC2_REQ_PARENT_ID]);
			
			if(row2[ASSOC2_REQ_MJ])
				parent_mj = atoi(row2[ASSOC2_REQ_MJ]);
			else
				parent_mj = -1;
			
			if(row2[ASSOC2_REQ_MNPJ])
				parent_mnpj = atoi(row2[ASSOC2_REQ_MNPJ]);
			else
				parent_mwpj = -1;
			
			if(row2[ASSOC2_REQ_MWPJ])
				parent_mwpj = atoi(row2[ASSOC2_REQ_MWPJ]);
			else
				parent_mwpj = -1;
			
			if(row2[ASSOC2_REQ_MCPJ])
				parent_mcpj = atoi(row2[ASSOC2_REQ_MCPJ]);
			else 
				parent_mcpj = -1;
			
			last_acct = row[ASSOC_REQ_ACCT];
			last_cluster = row[ASSOC_REQ_CLUSTER];
			mysql_free_result(result2);
		}
		if(row[ASSOC_REQ_MJ])
			assoc->max_jobs = atoi(row[ASSOC_REQ_MJ]);
		else
			assoc->max_jobs = parent_mj;
		if(row[ASSOC_REQ_MNPJ])
			assoc->max_nodes_per_job = 
				atoi(row[ASSOC_REQ_MNPJ]);
		else
			assoc->max_nodes_per_job = parent_mnpj;
		if(row[ASSOC_REQ_MWPJ])
			assoc->max_wall_duration_per_job = 
				atoi(row[ASSOC_REQ_MWPJ]);
		else
			assoc->max_wall_duration_per_job = parent_mwpj;
		if(row[ASSOC_REQ_MCPJ])
			assoc->max_cpu_secs_per_job = 
				atoi(row[ASSOC_REQ_MCPJ]);
		else
			assoc->max_cpu_secs_per_job = parent_mcpj;

		if(assoc->parent_id != acct_parent_id)
			assoc->parent_id = user_parent_id;
		//info("parent id is %d", assoc->parent_id);
		//log_assoc_rec(assoc);
	}
	mysql_free_result(result);

	return assoc_list;
#else
	return NULL;
#endif
}

extern int acct_storage_p_get_usage(mysql_conn_t *mysql_conn,
				    acct_usage_type_t type,
				    acct_association_rec_t *acct_assoc,
				    time_t start, time_t end)
{
#ifdef HAVE_MYSQL
	int rc = SLURM_SUCCESS;

	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int acct_storage_p_roll_usage(mysql_conn_t *mysql_conn, 
				     acct_usage_type_t type,
				     time_t start)
{
#ifdef HAVE_MYSQL
	int rc = SLURM_SUCCESS;

	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int clusteracct_storage_p_node_down(mysql_conn_t *mysql_conn, 
					   char *cluster,
					   struct node_record *node_ptr,
					   time_t event_time, char *reason)
{
#ifdef HAVE_MYSQL
	uint16_t cpus;
	int rc = SLURM_SUCCESS;
	char *query = NULL;
	char *my_reason;

	if (slurmctld_conf.fast_schedule && !slurmdbd_conf)
		cpus = node_ptr->config_ptr->cpus;
	else
		cpus = node_ptr->cpus;

	if (reason)
		my_reason = reason;
	else
		my_reason = node_ptr->reason;
	
	debug2("inserting %s(%s) with %u cpus", node_ptr->name, cluster, cpus);

	query = xstrdup_printf(
		"update %s set period_end=%d where cluster='%s' "
		"and period_end=0 and node_name='%s';",
		event_table, (event_time-1), cluster, node_ptr->name);
	xstrfmtcat(query,
		   "insert into %s "
		   "(node_name, cluster, cpu_count, period_start, reason) "
		   "values ('%s', '%s', %u, %d, '%s');",
		   event_table, node_ptr->name, cluster, 
		   cpus, event_time, my_reason);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);

	return rc;
#else
	return SLURM_ERROR;
#endif
}
extern int clusteracct_storage_p_node_up(mysql_conn_t *mysql_conn, 
					 char *cluster,
					 struct node_record *node_ptr,
					 time_t event_time)
{
#ifdef HAVE_MYSQL
	char* query;
	int rc = SLURM_SUCCESS;

	query = xstrdup_printf(
		"update %s set period_end=%d where cluster='%s' "
		"and period_end=0 and node_name='%s';",
		event_table, (event_time-1), cluster, node_ptr->name);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);
	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int clusteracct_storage_p_register_ctld(char *cluster,
					       uint16_t port)
{
	return SLURM_SUCCESS;
}

extern int clusteracct_storage_p_cluster_procs(mysql_conn_t *mysql_conn, 
					       char *cluster,
					       uint32_t procs,
					       time_t event_time)
{
#ifdef HAVE_MYSQL
	static uint32_t last_procs = -1;
	char* query;
	int rc = SLURM_SUCCESS;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	if (procs == last_procs) {
		debug3("we have the same procs as before no need to "
		       "update the database.");
		return SLURM_SUCCESS;
	}
	last_procs = procs;

	/* Record the processor count */
	query = xstrdup_printf(
		"select cpu_count from %s where cluster='%s' "
		"and period_end=0 and node_name=''",
		event_table, cluster);
	if(!(result = mysql_db_query_ret(
		     mysql_conn->acct_mysql_db, query, 0))) {
		xfree(query);
		return SLURM_ERROR;
	}
	xfree(query);

	/* we only are checking the first one here */
	if(!(row = mysql_fetch_row(result))) {
		debug("We don't have an entry for this machine %s "
		      "most likely a first time running.", cluster);
		goto add_it;
	}

	if(atoi(row[0]) == procs) {
		debug("%s hasn't changed since last entry", cluster);
		goto end_it;
	}
	debug("%s has changed from %s cpus to %u", cluster, row[0], procs);   

	query = xstrdup_printf(
		"update %s set period_end=%d where cluster='%s' "
		"and period_end=0 and node_name=''",
		event_table, (event_time-1), cluster);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);
	if(rc != SLURM_SUCCESS)
		goto end_it;
add_it:
	query = xstrdup_printf(
		"insert into %s (cluster, cpu_count, period_start) "
		"values ('%s', %u, %d)",
		event_table, cluster, procs, event_time);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);

end_it:
	mysql_free_result(result);
	return rc;
#else
	return SLURM_ERROR;
#endif
}

extern int clusteracct_storage_p_get_usage(
	mysql_conn_t *mysql_conn, acct_usage_type_t type, 
	acct_cluster_rec_t *cluster_rec, time_t start, time_t end)
{
#ifdef HAVE_MYSQL

	return SLURM_SUCCESS;
#else
	return SLURM_ERROR;
#endif
}

/* 
 * load into the storage the start of a job
 */
extern int jobacct_storage_p_job_start(mysql_conn_t *mysql_conn, 
				       struct job_record *job_ptr)
{
#ifdef HAVE_MYSQL
	int	rc=SLURM_SUCCESS;
	char	*jname, *nodes;
	long	priority;
	int track_steps = 0;
	char *block_id = NULL;
	char *query = NULL;
	int reinit = 0;

	if (!job_ptr->details || !job_ptr->details->submit_time) {
		error("jobacct_storage_p_job_start: "
		      "Not inputing this job, it has no submit time.");
		return SLURM_ERROR;
	}

	if(!mysql_conn) {
		error("We need a connection to run this");
		return SLURM_ERROR;
	} else if(!mysql_conn->acct_mysql_db
		  || mysql_ping(mysql_conn->acct_mysql_db) != 0) {
		if(!mysql_get_db_connection(&mysql_conn->acct_mysql_db,
					    mysql_db_name, mysql_db_info))
			return SLURM_ERROR;
	}
	
	
	debug2("mysql_jobacct_job_start() called");
	priority = (job_ptr->priority == NO_VAL) ?
		-1L : (long) job_ptr->priority;

	if (job_ptr->name && job_ptr->name[0]) {
		int i;
		jname = xmalloc(strlen(job_ptr->name) + 1);
		for (i=0; job_ptr->name[i]; i++) {
			if (isalnum(job_ptr->name[i]))
				jname[i] = job_ptr->name[i];
			else
				jname[i] = '_';
		}
	} else {
		jname = xstrdup("allocation");
		track_steps = 1;
	}

	if (job_ptr->nodes && job_ptr->nodes[0])
		nodes = job_ptr->nodes;
	else
		nodes = "(null)";

	if(job_ptr->batch_flag)
		track_steps = 1;

	if(slurmdbd_conf) {
		block_id = xstrdup(job_ptr->comment);
	} else {
		select_g_get_jobinfo(job_ptr->select_jobinfo, 
				     SELECT_DATA_BLOCK_ID, 
				     &block_id);
	}

	job_ptr->requid = -1; /* force to -1 for sacct to know this
			       * hasn't been set yet */
	if(!job_ptr->db_index) {
		query = xstrdup_printf(
			"insert into %s "
			"(jobid, account, associd, uid, gid, partition, "
			"blockid, eligible, submit, start, name, track_steps, "
			"state, priority, req_cpus, alloc_cpus, nodelist) "
			"values (%u, '%s', %u, %u, %u, '%s', '%s', "
			"%d, %d, %d, '%s', %u, "
			"%u, %u, %u, %u, '%s') "
			"on duplicate key update id=LAST_INSERT_ID(id)",
			job_table, job_ptr->job_id, job_ptr->account, 
			job_ptr->assoc_id,
			job_ptr->user_id, job_ptr->group_id,
			job_ptr->partition, block_id,
			(int)job_ptr->details->begin_time,
			(int)job_ptr->details->submit_time,
			(int)job_ptr->start_time,
			jname, track_steps,
			job_ptr->job_state & (~JOB_COMPLETING),
			priority, job_ptr->num_procs,
			job_ptr->total_procs, nodes);

	try_again:
		if(!(job_ptr->db_index = mysql_insert_ret_id(
			     mysql_conn->acct_mysql_db, query))) {
			if(!reinit) {
				error("It looks like the storage has gone "
				      "away trying to reconnect");
				mysql_close_db_connection(
					&mysql_conn->acct_mysql_db);
				mysql_get_db_connection(
					&mysql_conn->acct_mysql_db,
					mysql_db_name, mysql_db_info);
				reinit = 1;
				goto try_again;
			} else
				rc = SLURM_ERROR;
		}
	} else {
		query = xstrdup_printf(
			"update %s set partition='%s', blockid='%s', start=%d, "
			"name='%s', state=%u, alloc_cpus=%u, nodelist='%s', "
			"account='%s' where id=%d",
			job_table, job_ptr->partition, block_id,
			(int)job_ptr->start_time,
			jname, 
			job_ptr->job_state & (~JOB_COMPLETING),
			job_ptr->total_procs, nodes, 
			job_ptr->account, job_ptr->db_index);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	}

	xfree(block_id);
	xfree(jname);

	xfree(query);

	return rc;
#else
	return SLURM_ERROR;
#endif
}

/* 
 * load into the storage the end of a job
 */
extern int jobacct_storage_p_job_complete(mysql_conn_t *mysql_conn, 
					  struct job_record *job_ptr)
{
#ifdef HAVE_MYSQL
	char *query = NULL, *nodes = NULL;
	int rc=SLURM_SUCCESS;
	
	if (!job_ptr->db_index 
	    && (!job_ptr->details || !job_ptr->details->submit_time)) {
		error("jobacct_storage_p_job_complete: "
		      "Not inputing this job, it has no submit time.");
		return SLURM_ERROR;
	}

	if(!mysql_conn) {
		error("We need a connection to run this");
		return SLURM_ERROR;
	} else if(!mysql_conn->acct_mysql_db
		  || mysql_ping(mysql_conn->acct_mysql_db) != 0) {
		if(!mysql_get_db_connection(&mysql_conn->acct_mysql_db,
					    mysql_db_name, mysql_db_info))
			return SLURM_ERROR;
	}
	debug2("mysql_jobacct_job_complete() called");
	if (job_ptr->end_time == 0) {
		debug("mysql_jobacct: job %u never started", job_ptr->job_id);
		return SLURM_ERROR;
	}	
	
	if (job_ptr->nodes && job_ptr->nodes[0])
		nodes = job_ptr->nodes;
	else
		nodes = "(null)";

	if(!job_ptr->db_index) {
		job_ptr->db_index = _get_db_index(mysql_conn->acct_mysql_db,
						  job_ptr->details->submit_time,
						  job_ptr->job_id,
						  job_ptr->assoc_id);
		if(job_ptr->db_index == (uint32_t)-1) 
			return SLURM_ERROR;
	}
	query = xstrdup_printf("update %s set start=%u, end=%u, state=%d, "
			       "nodelist='%s', comp_code=%u, "
			       "kill_requid=%u where id=%u",
			       job_table, (int)job_ptr->start_time,
			       (int)job_ptr->end_time, 
			       job_ptr->job_state & (~JOB_COMPLETING),
			       nodes, job_ptr->exit_code,
			       job_ptr->requid, job_ptr->db_index);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);
	
	return  rc;
#else
	return SLURM_ERROR;
#endif
}

/* 
 * load into the storage the start of a job step
 */
extern int jobacct_storage_p_step_start(mysql_conn_t *mysql_conn, 
					struct step_record *step_ptr)
{
#ifdef HAVE_MYSQL
	int cpus = 0;
	int rc=SLURM_SUCCESS;
	char node_list[BUFFER_SIZE];
#ifdef HAVE_BG
	char *ionodes = NULL;
#endif
	char *query = NULL;
	
	if (!step_ptr->job_ptr->db_index 
	    && (!step_ptr->job_ptr->details
		|| !step_ptr->job_ptr->details->submit_time)) {
		error("jobacct_storage_p_step_start: "
		      "Not inputing this job, it has no submit time.");
		return SLURM_ERROR;
	}

	if(!mysql_conn) {
		error("We need a connection to run this");
		return SLURM_ERROR;
	} else if(!mysql_conn->acct_mysql_db
		  || mysql_ping(mysql_conn->acct_mysql_db) != 0) {
		if(!mysql_get_db_connection(&mysql_conn->acct_mysql_db,
					    mysql_db_name, mysql_db_info))
			return SLURM_ERROR;
	}
	if(slurmdbd_conf) {
		cpus = step_ptr->job_ptr->total_procs;
		snprintf(node_list, BUFFER_SIZE, "%s",
			 step_ptr->job_ptr->nodes);
	} else {
#ifdef HAVE_BG
		cpus = step_ptr->job_ptr->num_procs;
		select_g_get_jobinfo(step_ptr->job_ptr->select_jobinfo, 
				     SELECT_DATA_IONODES, 
				     &ionodes);
		if(ionodes) {
			snprintf(node_list, BUFFER_SIZE, 
				 "%s[%s]", step_ptr->job_ptr->nodes, ionodes);
			xfree(ionodes);
		} else
			snprintf(node_list, BUFFER_SIZE, "%s",
				 step_ptr->job_ptr->nodes);
		
#else
		if(!step_ptr->step_layout || !step_ptr->step_layout->task_cnt) {
			cpus = step_ptr->job_ptr->total_procs;
			snprintf(node_list, BUFFER_SIZE, "%s",
				 step_ptr->job_ptr->nodes);
		} else {
			cpus = step_ptr->step_layout->task_cnt;
			snprintf(node_list, BUFFER_SIZE, "%s", 
				 step_ptr->step_layout->node_list);
		}
#endif
	}

	step_ptr->job_ptr->requid = -1; /* force to -1 for sacct to know this
					 * hasn't been set yet  */

	if(!step_ptr->job_ptr->db_index) {
		step_ptr->job_ptr->db_index = 
			_get_db_index(mysql_conn->acct_mysql_db,
				      step_ptr->job_ptr->details->submit_time,
				      step_ptr->job_ptr->job_id,
				      step_ptr->job_ptr->assoc_id);
		if(step_ptr->job_ptr->db_index == (uint32_t)-1) 
			return SLURM_ERROR;
	}
	/* we want to print a -1 for the requid so leave it a
	   %d */
	query = xstrdup_printf(
		"insert into %s (id, stepid, start, name, state, "
		"cpus, nodelist) "
		"values (%d, %u, %d, '%s', %d, %u, '%s') "
		"on duplicate key update cpus=%u",
		step_table, step_ptr->job_ptr->db_index,
		step_ptr->step_id, 
		(int)step_ptr->start_time, step_ptr->name,
		JOB_RUNNING, cpus, node_list, cpus);
	debug3("%d query\n%s", mysql_conn->conn, query);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);

	return rc;
#else
	return SLURM_ERROR;
#endif
}

/* 
 * load into the storage the end of a job step
 */
extern int jobacct_storage_p_step_complete(mysql_conn_t *mysql_conn, 
					   struct step_record *step_ptr)
{
#ifdef HAVE_MYSQL
	time_t now;
	int elapsed;
	int comp_status;
	int cpus = 0;
	struct jobacctinfo *jobacct = (struct jobacctinfo *)step_ptr->jobacct;
	struct jobacctinfo dummy_jobacct;
	float ave_vsize = 0, ave_rss = 0, ave_pages = 0;
	float ave_cpu = 0, ave_cpu2 = 0;
	char *query = NULL;
	int rc =SLURM_SUCCESS;
	
	if (!step_ptr->job_ptr->db_index 
	    && (!step_ptr->job_ptr->details
		|| !step_ptr->job_ptr->details->submit_time)) {
		error("jobacct_storage_p_step_complete: "
		      "Not inputing this job, it has no submit time.");
		return SLURM_ERROR;
	}

	if (jobacct == NULL) {
		/* JobAcctGather=jobacct_gather/none, no data to process */
		bzero(&dummy_jobacct, sizeof(dummy_jobacct));
		jobacct = &dummy_jobacct;
	}

	if(!mysql_conn) {
		error("We need a connection to run this");
		return SLURM_ERROR;
	} else if(!mysql_conn->acct_mysql_db
		  || mysql_ping(mysql_conn->acct_mysql_db) != 0) {
		if(!mysql_get_db_connection(&mysql_conn->acct_mysql_db,
					    mysql_db_name, mysql_db_info))
			return SLURM_ERROR;
	}

	if(slurmdbd_conf) {
		now = step_ptr->job_ptr->end_time;
		cpus = step_ptr->job_ptr->total_procs;

	} else {
		now = time(NULL);
#ifdef HAVE_BG
		cpus = step_ptr->job_ptr->num_procs;
		
#else
		if(!step_ptr->step_layout || !step_ptr->step_layout->task_cnt)
			cpus = step_ptr->job_ptr->total_procs;
		else 
			cpus = step_ptr->step_layout->task_cnt;
#endif
	}
	
	if ((elapsed=now-step_ptr->start_time)<0)
		elapsed=0;	/* For *very* short jobs, if clock is wrong */
	if (step_ptr->exit_code)
		comp_status = JOB_FAILED;
	else
		comp_status = JOB_COMPLETE;

	/* figure out the ave of the totals sent */
	if(cpus > 0) {
		ave_vsize = jobacct->tot_vsize;
		ave_vsize /= cpus;
		ave_rss = jobacct->tot_rss;
		ave_rss /= cpus;
		ave_pages = jobacct->tot_pages;
		ave_pages /= cpus;
		ave_cpu = jobacct->tot_cpu;
		ave_cpu /= cpus;	
		ave_cpu /= 100;
	}
 
	if(jobacct->min_cpu != (uint32_t)NO_VAL) {
		ave_cpu2 = jobacct->min_cpu;
		ave_cpu2 /= 100;
	}

	if(!step_ptr->job_ptr->db_index) {
		step_ptr->job_ptr->db_index = 
			_get_db_index(mysql_conn->acct_mysql_db,
				      step_ptr->job_ptr->details->submit_time,
				      step_ptr->job_ptr->job_id,
				      step_ptr->job_ptr->assoc_id);
		if(step_ptr->job_ptr->db_index == -1) 
			return SLURM_ERROR;
	}

	query = xstrdup_printf(
		"update %s set end=%d, state=%d, "
		"kill_requid=%u, comp_code=%u, "
		"user_sec=%ld, user_usec=%ld, "
		"sys_sec=%ld, sys_usec=%ld, "
		"max_vsize=%u, max_vsize_task=%u, "
		"max_vsize_node=%u, ave_vsize=%.2f, "
		"max_rss=%u, max_rss_task=%u, "
		"max_rss_node=%u, ave_rss=%.2f, "
		"max_pages=%u, max_pages_task=%u, "
		"max_pages_node=%u, ave_pages=%.2f, "
		"min_cpu=%.2f, min_cpu_task=%u, "
		"min_cpu_node=%u, ave_cpu=%.2f "
		"where id=%u and stepid=%u",
		step_table, (int)now,
		comp_status,
		step_ptr->job_ptr->requid, 
		step_ptr->exit_code,
		/* user seconds */
		jobacct->user_cpu_sec,	
		/* user microseconds */
		jobacct->user_cpu_usec,
		/* system seconds */
		jobacct->sys_cpu_sec,
		/* system microsecs */
		jobacct->sys_cpu_usec,
		jobacct->max_vsize,	/* max vsize */
		jobacct->max_vsize_id.taskid,	/* max vsize task */
		jobacct->max_vsize_id.nodeid,	/* max vsize node */
		ave_vsize,	/* ave vsize */
		jobacct->max_rss,	/* max vsize */
		jobacct->max_rss_id.taskid,	/* max rss task */
		jobacct->max_rss_id.nodeid,	/* max rss node */
		ave_rss,	/* ave rss */
		jobacct->max_pages,	/* max pages */
		jobacct->max_pages_id.taskid,	/* max pages task */
		jobacct->max_pages_id.nodeid,	/* max pages node */
		ave_pages,	/* ave pages */
		ave_cpu2,	/* min cpu */
		jobacct->min_cpu_id.taskid,	/* min cpu task */
		jobacct->min_cpu_id.nodeid,	/* min cpu node */
		ave_cpu,	/* ave cpu */
		step_ptr->job_ptr->db_index, step_ptr->step_id);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	xfree(query);
	 
	return rc;
#else
	return SLURM_ERROR;
#endif
}

/* 
 * load into the storage a suspention of a job
 */
extern int jobacct_storage_p_suspend(mysql_conn_t *mysql_conn, 
				     struct job_record *job_ptr)
{
#ifdef HAVE_MYSQL
	char query[1024];
	int rc = SLURM_SUCCESS;
	
	if(!mysql_conn) {
		error("We need a connection to run this");
		return SLURM_ERROR;
	} else if(!mysql_conn->acct_mysql_db
		  || mysql_ping(mysql_conn->acct_mysql_db) != 0) {
		if(!mysql_get_db_connection(&mysql_conn->acct_mysql_db,
					    mysql_db_name, mysql_db_info))
			return SLURM_ERROR;
	}
	if(!job_ptr->db_index) {
		job_ptr->db_index = _get_db_index(mysql_conn->acct_mysql_db,
						  job_ptr->details->submit_time,
						  job_ptr->job_id,
						  job_ptr->assoc_id);
		if(job_ptr->db_index == -1) 
			return SLURM_ERROR;
	}

	snprintf(query, sizeof(query),
		 "update %s set suspended=%u-suspended, state=%d "
		 "where id=%u",
		 job_table, (int)job_ptr->suspend_time, 
		 job_ptr->job_state & (~JOB_COMPLETING),
		 job_ptr->db_index);
	rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	if(rc != SLURM_ERROR) {
		snprintf(query, sizeof(query),
			 "update %s set suspended=%u-suspended, "
			 "state=%d where id=%u and end=0",
			 step_table, (int)job_ptr->suspend_time, 
			 job_ptr->job_state, job_ptr->db_index);
		rc = mysql_db_query(mysql_conn->acct_mysql_db, query);
	}
	
	return rc;
#else
	return SLURM_ERROR;
#endif
}

/* 
 * get info from the storage 
 * returns List of job_rec_t *
 * note List needs to be freed when called
 */
extern List jobacct_storage_p_get_jobs(mysql_conn_t *mysql_conn, 
				       List selected_steps,
				       List selected_parts,
				       void *params)
{
	List job_list = NULL;
#ifdef HAVE_MYSQL
	if(!mysql_conn) {
		error("We need a connection to run this");
		return NULL;
	} else if(!mysql_conn->acct_mysql_db
		  || mysql_ping(mysql_conn->acct_mysql_db) != 0) {
		if(!mysql_get_db_connection(&mysql_conn->acct_mysql_db,
					    mysql_db_name, mysql_db_info))
			return NULL;
	}
	job_list = mysql_jobacct_process_get_jobs(mysql_conn,
						  selected_steps,
						  selected_parts,
						  params);	
#endif
	return job_list;
}

/* 
 * expire old info from the storage 
 */
extern void jobacct_storage_p_archive(mysql_conn_t *mysql_conn, 
				      List selected_parts,
				      void *params)
{
#ifdef HAVE_MYSQL
	if(!mysql_conn) {
		error("We need a connection to run this");
		return;
	} else if(!mysql_conn->acct_mysql_db
		  || mysql_ping(mysql_conn->acct_mysql_db) != 0) {
		if(!mysql_get_db_connection(&mysql_conn->acct_mysql_db,
					    mysql_db_name, mysql_db_info))
			return;
	}
	mysql_jobacct_process_archive(mysql_conn,
				      selected_parts, params);
#endif
	return;
}

extern int acct_storage_p_update_shares_used(void *db_conn,
					     List shares_used)
{
	/* This definitely needs to be fleshed out.
	 * Go through the list of shares_used_object_t objects and store them */
	return SLURM_SUCCESS;
}
