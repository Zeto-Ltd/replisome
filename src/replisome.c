/*-------------------------------------------------------------------------
 *
 * replisome.c
 * 		JSON output plugin for changeset extraction
 *
 * Copyright (c) 2013-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/replisome/replisome.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "replisome.h"
#include "reldata.h"
#include "jsonbutils.h"

#include "access/sysattr.h"

#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "catalog/index.h"

#include "nodes/parsenodes.h"

#include "replication/output_plugin.h"
#include "replication/logical.h"

#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

PG_MODULE_MAGIC;

extern void		_PG_init(void);
extern void		_PG_output_plugin_init(OutputPluginCallbacks *cb);


/* These must be available to pg_dlsym() */
static void rs_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init);
static void rs_decode_shutdown(LogicalDecodingContext *ctx);
static void rs_decode_begin_txn(LogicalDecodingContext *ctx,
					ReorderBufferTXN *txn);
static void rs_decode_commit_txn(LogicalDecodingContext *ctx,
					 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void rs_decode_change(LogicalDecodingContext *ctx,
				 ReorderBufferTXN *txn, Relation rel,
				 ReorderBufferChange *change);

static void output_begin(LogicalDecodingContext *ctx, JsonDecodingData *data,
		ReorderBufferTXN *txn, bool last_write);
void complete_table_details(JsonRelationEntry *entry, Relation relation,
		InclusionCommand *chosen_by);

void
_PG_init(void)
{
	/* Register the callback to receive schema changes */
	CacheRegisterRelcacheCallback(reldata_invalidate, (Datum)0);
}

/* Specify output plugin callbacks */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);

	cb->startup_cb = rs_decode_startup;
	cb->begin_cb = rs_decode_begin_txn;
	cb->change_cb = rs_decode_change;
	cb->commit_cb = rs_decode_commit_txn;
	cb->shutdown_cb = rs_decode_shutdown;
}

/* Initialize this plugin */
static void
rs_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
	ListCell	*option;
	JsonDecodingData *data;

	data = palloc0(sizeof(JsonDecodingData));
	data->context = AllocSetContextCreate(TopMemoryContext,
										"text conversion context",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);
	data->include_xids = false;
	data->include_timestamp = false;
	data->include_schemas = true;
	data->include_types = true;
	data->pretty_print = false;
	data->write_in_chunks = false;
	data->include_lsn = false;
	data->include_empty_xacts = false;
	data->commands = NULL;

	data->nr_changes = 0;

	data->reldata = reldata_create();

	ctx->output_plugin_private = data;

	opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;

	foreach(option, ctx->output_plugin_options)
	{
		DefElem *elem = lfirst(option);

		Assert(elem->arg == NULL || IsA(elem->arg, String));

		if (strcmp(elem->defname, "include-xids") == 0)
		{
			/* If option does not provide a value, it means its value is true */
			if (elem->arg == NULL)
			{
				elog(LOG, "include-xids argument is null");
				data->include_xids = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_xids))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-timestamp") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(LOG, "include-timestamp argument is null");
				data->include_timestamp = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_timestamp))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-schemas") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(LOG, "include-schemas argument is null");
				data->include_schemas = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_schemas))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-types") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(LOG, "include-types argument is null");
				data->include_types = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_types))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "pretty-print") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(LOG, "pretty-print argument is null");
				data->pretty_print = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->pretty_print))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "write-in-chunks") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(LOG, "write-in-chunks argument is null");
				data->write_in_chunks = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->write_in_chunks))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-lsn") == 0)
		{
			if (elem->arg == NULL)
			{
				elog(LOG, "include-lsn argument is null");
				data->include_lsn = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_lsn))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
							 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-empty-xacts") == 0)
		{

			if (elem->arg == NULL)
			{
				elog(LOG, "include-empty-xacts argument is null");
				data->include_empty_xacts = true;
			}
			else if (!parse_bool(strVal(elem->arg), &data->include_empty_xacts))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
						 strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include") == 0)
		{
			inc_parse_include(elem, &data->commands);
		}
		else if (strcmp(elem->defname, "exclude") == 0)
		{
			inc_parse_exclude(elem, &data->commands);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" = \"%s\" is unknown",
						elem->defname,
						elem->arg ? strVal(elem->arg) : "(null)")));
		}
	}
}

/* cleanup this plugin's resources */
static void
rs_decode_shutdown(LogicalDecodingContext *ctx)
{
	JsonDecodingData *data = ctx->output_plugin_private;

	/* cleanup our own resources via memory context reset */
	MemoryContextDelete(data->context);
}


/* BEGIN callback */
static void output_begin(LogicalDecodingContext *ctx, JsonDecodingData *data,
		ReorderBufferTXN *txn, bool last_write);

static void
rs_decode_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	JsonDecodingData *data = ctx->output_plugin_private;

	data->nr_changes = 0;

	reldata_to_invalidate(data->reldata);

	if (data->include_empty_xacts)
		output_begin(ctx, data, txn, true);
}

static void
output_begin(LogicalDecodingContext *ctx, JsonDecodingData *data,
		ReorderBufferTXN *txn, bool last_write)
{
	/* Transaction starts */
	OutputPluginPrepareWrite(ctx, last_write);

	if (data->pretty_print)
		appendStringInfoString(ctx->out, "{\n");
	else
		appendStringInfoChar(ctx->out, '{');

	if (data->include_xids)
	{
		if (data->pretty_print)
			appendStringInfo(ctx->out, "\t\"xid\": %u,\n", txn->xid);
		else
			appendStringInfo(ctx->out, "\"xid\":%u,", txn->xid);
	}

	if (data->include_lsn)
	{
		char *lsn_str = DatumGetCString(DirectFunctionCall1(pg_lsn_out, txn->end_lsn));

		if (data->pretty_print)
			appendStringInfo(ctx->out, "\t\"nextlsn\": \"%s\",\n", lsn_str);
		else
			appendStringInfo(ctx->out, "\"nextlsn\":\"%s\",", lsn_str);

		pfree(lsn_str);
	}

	if (data->include_timestamp)
	{
		if (data->pretty_print)
			appendStringInfo(ctx->out, "\t\"timestamp\": \"%s\",\n", timestamptz_to_str(txn->commit_time));
		else
			appendStringInfo(ctx->out, "\"timestamp\":\"%s\",", timestamptz_to_str(txn->commit_time));
	}

	if (data->pretty_print)
		appendStringInfoString(ctx->out, "\t\"change\": [");
	else
		appendStringInfoString(ctx->out, "\"change\":[");

	if (data->write_in_chunks)
		OutputPluginWrite(ctx, last_write);
}

/* COMMIT callback */
static void
rs_decode_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					 XLogRecPtr commit_lsn)
{
	JsonDecodingData *data = ctx->output_plugin_private;

	reldata_to_invalidate(NULL);

	if (txn->has_catalog_changes)
		elog(DEBUG1, "txn has catalog changes: yes");
	else
		elog(DEBUG1, "txn has catalog changes: no");
	elog(DEBUG1, "my change counter: %lu ; # of changes: %lu ; # of changes in memory: %lu", data->nr_changes, txn->nentries, txn->nentries_mem);
	elog(DEBUG1, "# of subxacts: %d", txn->nsubtxns);

	if (!data->include_empty_xacts && data->nr_changes == 0)
		return;

	/* Transaction ends */
	if (data->write_in_chunks)
		OutputPluginPrepareWrite(ctx, true);

	if (data->pretty_print)
	{
		/* if we don't write in chunks, we need a newline here */
		if (!data->write_in_chunks)
			appendStringInfoChar(ctx->out, '\n');

		appendStringInfoString(ctx->out, "\t]\n}");
	}
	else
	{
		appendStringInfoString(ctx->out, "]}");
	}

	OutputPluginWrite(ctx, true);
}

/*
 * Format a string as a JSON literal
 * XXX it doesn't do a sanity check for invalid input, does it?
 * FIXME it doesn't handle \uxxxx
 */
static void
quote_escape_json(StringInfo buf, const char *val)
{
	const char *valptr;

	appendStringInfoChar(buf, '"');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		/* XXX suppress \x in bytea field? */
		if (ch == '\\' && *(valptr + 1) == 'x')
		{
			valptr++;
			continue;
		}

		switch (ch)
		{
			case '"':
			case '\\':
			case '/':
				appendStringInfo(buf, "\\%c", ch);
				break;
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			default:
				appendStringInfoChar(buf, ch);
				break;
		}
	}
	appendStringInfoChar(buf, '"');
}

/*
 * Accumulate tuple information and stores it at the end
 *
 * replident: is this tuple a replica identity?
 * hasreplident: does this tuple has an associated replica identity?
 */
static void
tuple_to_stringinfo(LogicalDecodingContext *ctx, TupleDesc tupdesc, HeapTuple tuple, TupleDesc indexdesc, bool replident, bool hasreplident, bool include_schema, JsonRelationEntry *entry)
{
	JsonDecodingData	*data;
	int					natt;

	StringInfoData		colnames;
	StringInfoData		coltypes;
	StringInfoData		colvalues;
	char				*comma = "";

	bool				include_types;

	data = ctx->output_plugin_private;
	include_types = include_schema && data->include_types;

	initStringInfo(&colnames);
	if (include_types)
		initStringInfo(&coltypes);
	initStringInfo(&colvalues);

	/*
	 * If replident is true, it will output info about replica identity. In this
	 * case, there are special JSON objects for it. Otherwise, it will print new
	 * tuple data.
	 */
	if (replident)
	{
		if (data->pretty_print)
		{
			if (include_schema)
				appendStringInfoString(&colnames, "\t\t\t\"keynames\": [");
			if (include_types)
				appendStringInfoString(&coltypes, "\t\t\t\"keytypes\": [");
			appendStringInfoString(&colvalues, "\t\t\t\"oldkey\": [");
		}
		else
		{
			if (include_schema)
				appendStringInfoString(&colnames, "\"keynames\":[");
			if (include_types)
				appendStringInfoString(&coltypes, "\"keytypes\":[");
			appendStringInfoString(&colvalues, "\"oldkey\":[");
		}
	}
	else
	{
		if (data->pretty_print)
		{
			if (include_schema)
				appendStringInfoString(&colnames, "\t\t\t\"colnames\": [");
			if (include_types)
				appendStringInfoString(&coltypes, "\t\t\t\"coltypes\": [");
			appendStringInfoString(&colvalues, "\t\t\t\"values\": [");
		}
		else
		{
			if (include_schema)
				appendStringInfoString(&colnames, "\"colnames\":[");
			if (include_types)
				appendStringInfoString(&coltypes, "\"coltypes\":[");
			appendStringInfoString(&colvalues, "\"values\":[");
		}
	}

	/* Print column information (name, type, value) */
	for (natt = 0; natt < tupdesc->natts; natt++)
	{
		Form_pg_attribute	attr;		/* the attribute itself */
		Oid					typid;		/* type of current attribute */
		HeapTuple			type_tuple;	/* information about a type */
		Form_pg_type		type_form;
		Oid					typoutput;	/* output function */
		bool				typisvarlena;
		Datum				origval;	/* possibly toasted Datum */
		Datum				val;		/* definitely detoasted Datum */
		char				*outputstr = NULL;
		bool				isnull;		/* column is null? */

		attr = tupdesc->attrs[natt];

		elog(DEBUG1, "attribute \"%s\" (%d/%d)", NameStr(attr->attname), natt, tupdesc->natts);

		/* Do not print dropped or system columns */
		if (attr->attisdropped || attr->attnum < 0)
			continue;

		/* If there is a selection of columns skip the one not selected */
		if (entry->columns && !bms_is_member(natt, entry->columns)) {
			continue;
		}

		/* Search indexed columns in whole heap tuple */
		if (indexdesc != NULL)
		{
			int		j;
			bool	found_col = false;

			for (j = 0; j < indexdesc->natts; j++)
			{
				if (strcmp(NameStr(attr->attname), NameStr(indexdesc->attrs[j]->attname)) == 0)
					found_col = true;
			}

			/* Print only indexed columns */
			if (!found_col)
				continue;
		}

		typid = attr->atttypid;

		/* Figure out type name */
		type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(type_tuple))
			elog(ERROR, "cache lookup failed for type %u", typid);
		type_form = (Form_pg_type) GETSTRUCT(type_tuple);

		/* Get information needed for printing values of a type */
		getTypeOutputInfo(typid, &typoutput, &typisvarlena);

		/* Get Datum from tuple */
		origval = heap_getattr(tuple, natt + 1, tupdesc, &isnull);

		/* Skip nulls iif printing key/identity */
		if (isnull && replident)
			continue;

		/* XXX Unchanged TOAST Datum does not need to be output */
		if (!isnull && typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval))
		{
			elog(WARNING, "column \"%s\" has an unchanged TOAST", NameStr(attr->attname));
			continue;
		}

		/* Accumulate each column info */
		if (include_schema)
			appendStringInfo(&colnames, "%s\"%s\"", comma, NameStr(attr->attname));

		if (include_types)
			appendStringInfo(&coltypes, "%s\"%s\"", comma, NameStr(type_form->typname));

		ReleaseSysCache(type_tuple);

		if (isnull)
		{
			appendStringInfo(&colvalues, "%snull", comma);
		}
		else
		{
			if (typisvarlena)
				val = PointerGetDatum(PG_DETOAST_DATUM(origval));
			else
				val = origval;

			/* Finally got the value */
			outputstr = OidOutputFunctionCall(typoutput, val);

			/*
			 * Data types are printed with quotes unless they are number, true,
			 * false, null, an array or an object.
			 *
			 * The NaN and Infinity are not valid JSON symbols. Hence,
			 * regardless of sign they are represented as the string null.
			 */
			switch (typid)
			{
				case INT2OID:
				case INT4OID:
				case INT8OID:
				case OIDOID:
				case FLOAT4OID:
				case FLOAT8OID:
				case NUMERICOID:
					if (pg_strncasecmp(outputstr, "NaN", 3) == 0 ||
							pg_strncasecmp(outputstr, "Infinity", 8) == 0 ||
							pg_strncasecmp(outputstr, "-Infinity", 9) == 0)
					{
						appendStringInfo(&colvalues, "%snull", comma);
						elog(DEBUG1, "attribute \"%s\" is special: %s", NameStr(attr->attname), outputstr);
					}
					else if (strspn(outputstr, "0123456789+-eE.") == strlen(outputstr))
						appendStringInfo(&colvalues, "%s%s", comma, outputstr);
					else
						elog(ERROR, "%s is not a number", outputstr);
					break;
				case BOOLOID:
					if (strcmp(outputstr, "t") == 0)
						appendStringInfo(&colvalues, "%strue", comma);
					else
						appendStringInfo(&colvalues, "%sfalse", comma);
					break;
				default:
					appendStringInfoString(&colvalues, comma);
					quote_escape_json(&colvalues, outputstr);
					break;
			}
		}

		/* The first column does not have comma */
		if (strcmp(comma, "") == 0)
		{
			if (data->pretty_print)
				comma = ", ";
			else
				comma = ",";
		}
	}

	/* Column info ends */
	if (replident)
	{
		if (data->pretty_print)
		{
			if (include_schema)
				appendStringInfoString(&colnames, "],\n");
			if (include_types)
				appendStringInfoString(&coltypes, "],\n");
			appendStringInfoString(&colvalues, "]\n");
		}
		else
		{
			if (include_schema)
				appendStringInfoString(&colnames, "],");
			if (include_types)
				appendStringInfoString(&coltypes, "],");
			appendStringInfoChar(&colvalues, ']');
		}
	}
	else
	{
		if (data->pretty_print)
		{
			if (include_schema)
				appendStringInfoString(&colnames, "],\n");
			if (include_types)
				appendStringInfoString(&coltypes, "],\n");
			if (hasreplident)
				appendStringInfoString(&colvalues, "],\n");
			else
				appendStringInfoString(&colvalues, "]\n");
		}
		else
		{
			if (include_schema)
				appendStringInfoString(&colnames, "],");
			if (include_types)
				appendStringInfoString(&coltypes, "],");
			if (hasreplident)
				appendStringInfoString(&colvalues, "],");
			else
				appendStringInfoChar(&colvalues, ']');
		}
	}

	/* Print data */
	appendStringInfoString(ctx->out, colnames.data);
	if (include_types)
		appendStringInfoString(ctx->out, coltypes.data);
	appendStringInfoString(ctx->out, colvalues.data);

	pfree(colnames.data);
	if (include_types)
		pfree(coltypes.data);
	pfree(colvalues.data);
}

/* Print columns information */
static void
columns_to_stringinfo(LogicalDecodingContext *ctx, TupleDesc tupdesc, HeapTuple tuple, bool hasreplident, JsonRelationEntry *entry)
{
	bool include_schema = !entry->names_emitted;
	tuple_to_stringinfo(ctx, tupdesc, tuple, NULL, false, hasreplident, include_schema, entry);
}

/* Print replica identity information */
static void
identity_to_stringinfo(LogicalDecodingContext *ctx, TupleDesc tupdesc, HeapTuple tuple, TupleDesc indexdesc, JsonRelationEntry *entry)
{
	bool include_schema = !entry->key_emitted;

	/* hasreplident=false parameter does not matter */
	tuple_to_stringinfo(ctx, tupdesc, tuple, indexdesc, true, false, include_schema, entry);
}

/* Callback for individual changed tuples */
void
rs_decode_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				 Relation relation, ReorderBufferChange *change)
{
	JsonDecodingData *data;
	Form_pg_class class_form;
	TupleDesc	tupdesc;
	MemoryContext old;

	Relation	indexrel;
	TupleDesc	indexdesc;
	JsonRelationEntry *entry;

	AssertVariableIsOfType(&rs_decode_change, LogicalDecodeChangeCB);

	data = ctx->output_plugin_private;

	/* Look up or insert a new entry in the cache */
	entry = reldata_enter(data->reldata, relation->rd_id);

	/* check if we have to emit this table */
	if (entry->exclude) {
		return;
	}
	else if (!entry->include) {
		InclusionCommand *chosen_by;
		entry->include = inc_should_emit(data->commands, relation, &chosen_by);
		if (!entry->include) {
			entry->exclude = true;
			return;
		}
		else {
			complete_table_details(entry, relation, chosen_by);
		}
	}

	class_form = RelationGetForm(relation);
	tupdesc = RelationGetDescr(relation);

	/* Avoid leaking memory by using and resetting our own context */
	old = MemoryContextSwitchTo(data->context);

	/* Make sure rd_replidindex is set */
	RelationGetIndexList(relation);

	/* Sanity checks */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			if (change->data.tp.newtuple == NULL)
			{
				elog(WARNING, "no tuple data for INSERT in table \"%s\"", NameStr(class_form->relname));
				goto reset_ctx;
			}
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			/*
			 * Bail out iif:
			 * (i) doesn't have a pk and replica identity is not full;
			 * (ii) replica identity is nothing.
			 */
			if (!OidIsValid(relation->rd_replidindex) && relation->rd_rel->relreplident != REPLICA_IDENTITY_FULL)
			{
				/* FIXME this sentence is imprecise */
				elog(WARNING, "table \"%s\" without primary key or replica identity is nothing", NameStr(class_form->relname));
				goto reset_ctx;
			}

			if (change->data.tp.newtuple == NULL)
			{
				elog(WARNING, "no tuple data for UPDATE in table \"%s\"", NameStr(class_form->relname));
				goto reset_ctx;
			}
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			/*
			 * Bail out iif:
			 * (i) doesn't have a pk and replica identity is not full;
			 * (ii) replica identity is nothing.
			 */
			if (!OidIsValid(relation->rd_replidindex) && relation->rd_rel->relreplident != REPLICA_IDENTITY_FULL)
			{
				/* FIXME this sentence is imprecise */
				elog(WARNING, "table \"%s\" without primary key or replica identity is nothing", NameStr(class_form->relname));
				goto reset_ctx;
			}

			if (change->data.tp.oldtuple == NULL)
			{
				elog(WARNING, "no tuple data for DELETE in table \"%s\"", NameStr(class_form->relname));
				goto reset_ctx;
			}
			break;
		default:
			Assert(false);
	}

	if (!data->include_empty_xacts && data->nr_changes == 0)
		output_begin(ctx, data, txn, false);

	if (data->write_in_chunks)
		OutputPluginPrepareWrite(ctx, true);

	/* Change counter */
	data->nr_changes++;

	/* Change starts */
	if (data->pretty_print)
	{
		/* if we don't write in chunks, we need a newline here */
		if (!data->write_in_chunks)
			appendStringInfoChar(ctx->out, '\n');

		appendStringInfoString(ctx->out, "\t\t");

		if (data->nr_changes > 1)
			appendStringInfoChar(ctx->out, ',');

		appendStringInfoString(ctx->out, "{\n");
	}
	else
	{
		if (data->nr_changes > 1)
			appendStringInfoString(ctx->out, ",{");
		else
			appendStringInfoChar(ctx->out, '{');
	}

	/* Print change kind */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			if (data->pretty_print)
				appendStringInfoString(ctx->out, "\t\t\t\"op\": \"I\",\n");
			else
				appendStringInfoString(ctx->out, "\"op\":\"I\",");
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			if(data->pretty_print)
				appendStringInfoString(ctx->out, "\t\t\t\"op\": \"U\",\n");
			else
				appendStringInfoString(ctx->out, "\"op\":\"U\",");
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			if (data->pretty_print)
				appendStringInfoString(ctx->out, "\t\t\t\"op\": \"D\",\n");
			else
				appendStringInfoString(ctx->out, "\"op\":\"D\",");
			break;
		default:
			Assert(false);
	}

	/* Print table name (possibly) qualified */
	if (data->pretty_print)
	{
		if (data->include_schemas)
			appendStringInfo(ctx->out, "\t\t\t\"schema\": \"%s\",\n", get_namespace_name(class_form->relnamespace));
		appendStringInfo(ctx->out, "\t\t\t\"table\": \"%s\",\n", NameStr(class_form->relname));
	}
	else
	{
		if (data->include_schemas)
			appendStringInfo(ctx->out, "\"schema\":\"%s\",", get_namespace_name(class_form->relnamespace));
		appendStringInfo(ctx->out, "\"table\":\"%s\",", NameStr(class_form->relname));
	}

	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			/* Print the new tuple */
			columns_to_stringinfo(ctx, tupdesc, &change->data.tp.newtuple->tuple, false, entry);
			entry->names_emitted = true;
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			/* Print the new tuple */
			columns_to_stringinfo(ctx, tupdesc, &change->data.tp.newtuple->tuple, true, entry);
			entry->names_emitted = true;

			/*
			 * The old tuple is available when:
			 * (i) pk changes;
			 * (ii) replica identity is full;
			 * (iii) replica identity is index and indexed column changes.
			 *
			 * FIXME if old tuple is not available we must get only the indexed
			 * columns (the whole tuple is printed).
			 */
			if (change->data.tp.oldtuple == NULL)
			{
				elog(DEBUG1, "old tuple is null");

				indexrel = RelationIdGetRelation(relation->rd_replidindex);
				if (indexrel != NULL)
				{
					indexdesc = RelationGetDescr(indexrel);
					identity_to_stringinfo(ctx, tupdesc, &change->data.tp.newtuple->tuple, indexdesc, entry);
					RelationClose(indexrel);
				}
				else
				{
					identity_to_stringinfo(ctx, tupdesc, &change->data.tp.newtuple->tuple, NULL, entry);
				}
			}
			else
			{
				elog(DEBUG1, "old tuple is not null");
				identity_to_stringinfo(ctx, tupdesc, &change->data.tp.oldtuple->tuple, NULL, entry);
			}
			entry->key_emitted = true;
			break;

		case REORDER_BUFFER_CHANGE_DELETE:
			/* Print the replica identity */
			indexrel = RelationIdGetRelation(relation->rd_replidindex);
			if (indexrel != NULL)
			{
				indexdesc = RelationGetDescr(indexrel);
				identity_to_stringinfo(ctx, tupdesc, &change->data.tp.oldtuple->tuple, indexdesc, entry);
				RelationClose(indexrel);
			}
			else
			{
				identity_to_stringinfo(ctx, tupdesc, &change->data.tp.oldtuple->tuple, NULL, entry);
			}
			entry->key_emitted = true;

			if (change->data.tp.oldtuple == NULL)
				elog(DEBUG1, "old tuple is null");
			else
				elog(DEBUG1, "old tuple is not null");
			break;

		default:
			Assert(false);
	}

	if (data->pretty_print)
		appendStringInfoString(ctx->out, "\t\t}");
	else
		appendStringInfoChar(ctx->out, '}');

	if (data->write_in_chunks)
		OutputPluginWrite(ctx, true);

reset_ctx:
	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

void
complete_table_details(JsonRelationEntry *entry, Relation relation, InclusionCommand *chosen_by)
{
	TupleDesc tupdesc;
	Form_pg_attribute attr;
	int natt;

	if (!chosen_by)
		return;

	tupdesc = RelationGetDescr(relation);

	if (chosen_by->columns) {
		int i, ncols;
		ncols = jbu_array_len(chosen_by->columns);
		for (i = 0; i < ncols; i ++) {
			char *want = jbu_getitem_str(chosen_by->columns, i);

			for (natt = 0; natt < tupdesc->natts; natt++) {
				attr = tupdesc->attrs[natt];
				if (attr->attisdropped || attr->attnum < 0)
					continue;

				if (strcmp(NameStr(attr->attname), want) == 0) {
					elog(DEBUG1, "want column %s is the number %i",
						want, natt);
					entry->columns = bms_add_member(entry->columns, natt);
					break;
				}
			}
		}
	}

	if (chosen_by->skip_columns) {
		int i, ncols;

		/* Select all the valid columns */
		for (natt = 0; natt < tupdesc->natts; natt++) {
			attr = tupdesc->attrs[natt];
			if (attr->attisdropped || attr->attnum < 0)
				continue;

			entry->columns = bms_add_member(entry->columns, natt);
		}

		ncols = jbu_array_len(chosen_by->skip_columns);
		for (i = 0; i < ncols; i ++) {
			char *want = jbu_getitem_str(chosen_by->skip_columns, i);

			for (natt = 0; natt < tupdesc->natts; natt++) {
				attr = tupdesc->attrs[natt];
				if (attr->attisdropped || attr->attnum < 0)
					continue;

				if (strcmp(NameStr(attr->attname), want) == 0) {
					elog(DEBUG1, "unwanted column %s is the number %i",
						want, natt);
					entry->columns = bms_del_member(entry->columns, natt);
					break;
				}
			}
		}
	}
}
