/*-------------------------------------------------------------------------
 *
 * hypopg_import_table.c: Import of some PostgreSQL private fuctions, used
 *                        for hypothetical partitioning.
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 * Copyright (c) 2008-2018, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#if PG_VERSION_NUM >= 100000

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/var.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#if PG_VERSION_NUM >= 110000
#include "utils/rel.h"
#endif
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#if PG_VERSION_NUM >= 110000
#include "partitioning/partbounds.h"
#include "partitioning/partdefs.h"
#include "utils/partcache.h"
#endif

#include "include/hypopg_import_table.h"

/* pg10 only imports */
#if PG_VERSION_NUM < 110000
static int32 partition_rbound_datum_cmp(PartitionKey key,
						   Datum *rb_datums, PartitionRangeDatumKind *rb_kind,
						   Datum *tuple_datums);

/*
 * Copied from src/backend/catalog/partition.c
 *
 * make_one_range_bound
 *
 * Return a PartitionRangeBound given a list of PartitionRangeDatum elements
 * and a flag telling whether the bound is lower or not.  Made into a function
 * because there are multiple sites that want to use this facility.
 */
PartitionRangeBound *
make_one_range_bound(PartitionKey key, int index, List *datums, bool lower)
{
	PartitionRangeBound *bound;
	ListCell   *lc;
	int			i;

	bound = (PartitionRangeBound *) palloc0(sizeof(PartitionRangeBound));
	bound->index = index;
	bound->datums = (Datum *) palloc0(key->partnatts * sizeof(Datum));
	bound->kind = (PartitionRangeDatumKind *) palloc0(key->partnatts *
													  sizeof(PartitionRangeDatumKind));
	bound->lower = lower;

	i = 0;
	foreach(lc, datums)
	{
		PartitionRangeDatum *datum = castNode(PartitionRangeDatum, lfirst(lc));

		/* What's contained in this range datum? */
		bound->kind[i] = datum->kind;

		if (datum->kind == PARTITION_RANGE_DATUM_VALUE)
		{
			Const	   *val = castNode(Const, datum->value);

			if (val->constisnull)
				elog(ERROR, "invalid range bound datum");
			bound->datums[i] = val->constvalue;
		}

		i++;
	}

	return bound;
}

/*
 * Copied from src/backend/catalog/partition.c
 *
 * Binary search on a collection of partition bounds. Returns greatest
 * bound in array boundinfo->datums which is less than or equal to *probe.
 * If all bounds in the array are greater than *probe, -1 is returned.
 *
 * *probe could either be a partition bound or a Datum array representing
 * the partition key of a tuple being routed; probe_is_bound tells which.
 * We pass that down to the comparison function so that it can interpret the
 * contents of *probe accordingly.
 *
 * *is_equal is set to whether the bound at the returned index is equal with
 * *probe.
 */
int
partition_bound_bsearch(PartitionKey key, PartitionBoundInfo boundinfo,
						void *probe, bool probe_is_bound, bool *is_equal)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		int32		cmpval;

		mid = (lo + hi + 1) / 2;
		cmpval = partition_bound_cmp(key, boundinfo, mid, probe,
									 probe_is_bound);
		if (cmpval <= 0)
		{
			lo = mid;
			*is_equal = (cmpval == 0);

			if (*is_equal)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * Copied from src/backend/catalog/partition.c
 *
 * partition_bound_cmp
 *
 * Return whether the bound at offset in boundinfo is <, =, or > the argument
 * specified in *probe.
 */
int32
partition_bound_cmp(PartitionKey key, PartitionBoundInfo boundinfo,
					int offset, void *probe, bool probe_is_bound)
{
	Datum	   *bound_datums = boundinfo->datums[offset];
	int32		cmpval = -1;

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_LIST:
			cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[0],
													 key->partcollation[0],
													 bound_datums[0],
													 *(Datum *) probe));
			break;

		case PARTITION_STRATEGY_RANGE:
			{
				PartitionRangeDatumKind *kind = boundinfo->kind[offset];

				if (probe_is_bound)
				{
					/*
					 * We need to pass whether the existing bound is a lower
					 * bound, so that two equal-valued lower and upper bounds
					 * are not regarded equal.
					 */
					bool		lower = boundinfo->indexes[offset] < 0;

					cmpval = partition_rbound_cmp(key,
												  bound_datums, kind, lower,
												  (PartitionRangeBound *) probe);
				}
				else
					cmpval = partition_rbound_datum_cmp(key,
														bound_datums, kind,
														(Datum *) probe);
				break;
			}

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
	}

	return cmpval;
}

/*
 * Imported from src/backend/catalog/partition.c, not exported in pg10
 *
 * partition_rbound_cmp
 *
 * Return for two range bounds whether the 1st one (specified in datum1,
 * kind1, and lower1) is <, =, or > the bound specified in *b2.
 *
 * Note that if the values of the two range bounds compare equal, then we take
 * into account whether they are upper or lower bounds, and an upper bound is
 * considered to be smaller than a lower bound. This is important to the way
 * that RelationBuildPartitionDesc() builds the PartitionBoundInfoData
 * structure, which only stores the upper bound of a common boundary between
 * two contiguous partitions.
 */
int32
partition_rbound_cmp(PartitionKey key,
					 Datum *datums1, PartitionRangeDatumKind *kind1,
					 bool lower1, PartitionRangeBound *b2)
{
	int32		cmpval = 0;		/* placate compiler */
	int			i;
	Datum	   *datums2 = b2->datums;
	PartitionRangeDatumKind *kind2 = b2->kind;
	bool		lower2 = b2->lower;

	for (i = 0; i < key->partnatts; i++)
	{
		/*
		 * First, handle cases where the column is unbounded, which should not
		 * invoke the comparison procedure, and should not consider any later
		 * columns. Note that the PartitionRangeDatumKind enum elements
		 * compare the same way as the values they represent.
		 */
		if (kind1[i] < kind2[i])
			return -1;
		else if (kind1[i] > kind2[i])
			return 1;
		else if (kind1[i] != PARTITION_RANGE_DATUM_VALUE)

			/*
			 * The column bounds are both MINVALUE or both MAXVALUE. No later
			 * columns should be considered, but we still need to compare
			 * whether they are upper or lower bounds.
			 */
			break;

		cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[i],
												 key->partcollation[i],
												 datums1[i],
												 datums2[i]));
		if (cmpval != 0)
			break;
	}

	/*
	 * If the comparison is anything other than equal, we're done. If they
	 * compare equal though, we still have to consider whether the boundaries
	 * are inclusive or exclusive.  Exclusive one is considered smaller of the
	 * two.
	 */
	if (cmpval == 0 && lower1 != lower2)
		cmpval = lower1 ? 1 : -1;

	return cmpval;
}

/*
 * Imported from src/backend/catalog/partition.c, not exported in pg10
 *
 * partition_rbound_datum_cmp
 *
 * Return whether range bound (specified in rb_datums, rb_kind, and rb_lower)
 * is <, =, or > partition key of tuple (tuple_datums)
 */
static int32
partition_rbound_datum_cmp(PartitionKey key,
						   Datum *rb_datums, PartitionRangeDatumKind *rb_kind,
						   Datum *tuple_datums)
{
	int			i;
	int32		cmpval = -1;

	for (i = 0; i < key->partnatts; i++)
	{
		if (rb_kind[i] == PARTITION_RANGE_DATUM_MINVALUE)
			return -1;
		else if (rb_kind[i] == PARTITION_RANGE_DATUM_MAXVALUE)
			return 1;

		cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[i],
												 key->partcollation[i],
												 rb_datums[i],
												 tuple_datums[i]));
		if (cmpval != 0)
			break;
	}

	return cmpval;
}
#endif		/* pg10 only imports */

/*
 * Copied from src/backend/commands/tablecmds.c, not exported.
 *
 * Transform any expressions present in the partition key
 *
 * Returns a transformed PartitionSpec, as well as the strategy code
 */
PartitionSpec *
transformPartitionSpec(Relation rel, PartitionSpec *partspec, char *strategy)
{
	PartitionSpec *newspec;
	ParseState *pstate;
	RangeTblEntry *rte;
	ListCell   *l;

	newspec = makeNode(PartitionSpec);

	newspec->strategy = partspec->strategy;
	newspec->partParams = NIL;
	newspec->location = partspec->location;

	/* Parse partitioning strategy name */
#if PG_VERSION_NUM >= 110000
	if (pg_strcasecmp(partspec->strategy, "hash") == 0)
		*strategy = PARTITION_STRATEGY_HASH;
	else
#endif
	if (pg_strcasecmp(partspec->strategy, "list") == 0)
		*strategy = PARTITION_STRATEGY_LIST;
	else if (pg_strcasecmp(partspec->strategy, "range") == 0)
		*strategy = PARTITION_STRATEGY_RANGE;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized partitioning strategy \"%s\"",
						partspec->strategy)));

	/* Check valid number of columns for strategy */
	if (*strategy == PARTITION_STRATEGY_LIST &&
		list_length(partspec->partParams) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("cannot use \"list\" partition strategy with more than one column")));

	/*
	 * Create a dummy ParseState and insert the target relation as its sole
	 * rangetable entry.  We need a ParseState for transformExpr.
	 */
	pstate = make_parsestate(NULL);
	rte = addRangeTableEntryForRelation(pstate, rel,
#if PG_VERSION_NUM >= 120000
			AccessShareLock,
#endif
			NULL, false, true);
	addRTEtoQuery(pstate, rte, true, true, true);

	/* take care of any partition expressions */
	foreach(l, partspec->partParams)
	{
		PartitionElem *pelem = castNode(PartitionElem, lfirst(l));
		ListCell   *lc;

		/* Check for PARTITION BY ... (foo, foo) */
		foreach(lc, newspec->partParams)
		{
			PartitionElem *pparam = castNode(PartitionElem, lfirst(lc));

			if (pelem->name && pparam->name &&
				strcmp(pelem->name, pparam->name) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" appears more than once in partition key",
								pelem->name),
						 parser_errposition(pstate, pelem->location)));
		}

		if (pelem->expr)
		{
			/* Copy, to avoid scribbling on the input */
			pelem = copyObject(pelem);

			/* Now do parse transformation of the expression */
			pelem->expr = transformExpr(pstate, pelem->expr,
										EXPR_KIND_PARTITION_EXPRESSION);

			/* we have to fix its collations too */
			assign_expr_collations(pstate, pelem->expr);
		}

		newspec->partParams = lappend(newspec->partParams, pelem);
	}

	return newspec;
}

/*
 * Copied from src/backend/commands/tablecmds.c, not exported.
 *
 * Compute per-partition-column information from a list of PartitionElems.
 * Expressions in the PartitionElems must be parse-analyzed already.
 */
void
ComputePartitionAttrs(Relation rel, List *partParams, AttrNumber *partattrs,
					  List **partexprs, Oid *partopclass, Oid *partcollation,
					  char strategy)
{
	int			attn;
	ListCell   *lc;
	Oid			am_oid;

	attn = 0;
	foreach(lc, partParams)
	{
		PartitionElem *pelem = castNode(PartitionElem, lfirst(lc));
		Oid			atttype;
		Oid			attcollation;

		if (pelem->name != NULL)
		{
			/* Simple attribute reference */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			atttuple = SearchSysCacheAttName(RelationGetRelid(rel),
											 pelem->name);
			if (!HeapTupleIsValid(atttuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in partition key does not exist",
								pelem->name)));
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);

			if (attform->attnum <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot use system column \"%s\" in partition key",
								pelem->name)));

			partattrs[attn] = attform->attnum;
			atttype = attform->atttypid;
			attcollation = attform->attcollation;
			ReleaseSysCache(atttuple);
		}
		else
		{
			/* Expression */
			Node	   *expr = pelem->expr;

			Assert(expr != NULL);
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 * Strip any top-level COLLATE clause.  This ensures that we treat
			 * "x COLLATE y" and "(x COLLATE y)" alike.
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			if (IsA(expr, Var) &&
				((Var *) expr)->varattno > 0)
			{
				/*
				 * User wrote "(column)" or "(column COLLATE something)".
				 * Treat it like simple attribute anyway.
				 */
				partattrs[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				Bitmapset  *expr_attrs = NULL;
				int			i;

				partattrs[attn] = 0;	/* marks the column as expression */
				*partexprs = lappend(*partexprs, expr);

				/*
				 * Try to simplify the expression before checking for
				 * mutability.  The main practical value of doing it in this
				 * order is that an inline-able SQL-language function will be
				 * accepted if its expansion is immutable, whether or not the
				 * function itself is marked immutable.
				 *
				 * Note that expression_planner does not change the passed in
				 * expression destructively and we have already saved the
				 * expression to be stored into the catalog above.
				 */
				expr = (Node *) expression_planner((Expr *) expr);

				/*
				 * Partition expression cannot contain mutable functions,
				 * because a given row must always map to the same partition
				 * as long as there is no change in the partition boundary
				 * structure.
				 */
				if (contain_mutable_functions(expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in partition key expression must be marked IMMUTABLE")));

				/*
				 * transformPartitionSpec() should have already rejected
				 * subqueries, aggregates, window functions, and SRFs, based
				 * on the EXPR_KIND_ for partition expressions.
				 */

				/*
				 * Cannot have expressions containing whole-row references or
				 * system column references.
				 */
				pull_varattnos(expr, 1, &expr_attrs);
				if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber,
								  expr_attrs))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("partition key expressions cannot contain whole-row references")));
				for (i = FirstLowInvalidHeapAttributeNumber; i < 0; i++)
				{
					if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
									  expr_attrs))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("partition key expressions cannot contain system column references")));
				}

				/*
				 * While it is not exactly *wrong* for a partition expression
				 * to be a constant, it seems better to reject such keys.
				 */
				if (IsA(expr, Const))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("cannot use constant expression as partition key")));
			}
		}

		/*
		 * Apply collation override if any
		 */
		if (pelem->collation)
			attcollation = get_collation_oid(pelem->collation, false);

		/*
		 * Check we have a collation iff it's a collatable type.  The only
		 * expected failures here are (1) COLLATE applied to a noncollatable
		 * type, or (2) partition expression had an unresolved collation. But
		 * we might as well code this to be a complete consistency check.
		 */
		if (type_is_collatable(atttype))
		{
			if (!OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for partition expression"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}
		else
		{
			if (OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collations are not supported by type %s",
								format_type_be(atttype))));
		}

		partcollation[attn] = attcollation;

#if PG_VERSION_NUM >= 110000
		/*
		 * Identify the appropriate operator class.  For list and range
		 * partitioning, we use a btree operator class; hash partitioning uses
		 * a hash operator class.
		 */
		if (strategy == PARTITION_STRATEGY_HASH)
			am_oid = HASH_AM_OID;
		else
#endif
			am_oid = BTREE_AM_OID;

		if (!pelem->opclass)
		{
			partopclass[attn] = GetDefaultOpClass(atttype, am_oid);

			if (!OidIsValid(partopclass[attn]))
			{
#if PG_VERSION_NUM >= 110000
				if (strategy == PARTITION_STRATEGY_HASH)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("data type %s has no default hash operator class",
									format_type_be(atttype)),
							 errhint("You must specify a hash operator class or define a default hash operator class for the data type.")));
				else
#endif
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("data type %s has no default btree operator class",
									format_type_be(atttype)),
							 errhint("You must specify a btree operator class or define a default btree operator class for the data type.")));

			}
		}
		else
			partopclass[attn] = ResolveOpClass(pelem->opclass,
											   atttype,
											   am_oid == HASH_AM_OID ? "hash" : "btree",
											   am_oid);

		attn++;
	}
}

/*
 * Copied from src/backend/utils/adt/ruleutils.c, not exported.
 *
 * get_relation_name
 *		Get the unqualified name of a relation specified by OID
 *
 * This differs from the underlying get_rel_name() function in that it will
 * throw error instead of silently returning NULL if the OID is bad.
 */
char *
get_relation_name(Oid relid)
{
	char	   *relname = get_rel_name(relid);

	if (!relname)
		elog(ERROR, "cache lookup failed for relation %u", relid);
	return relname;
}

/*
 * Copied from src/backend/utils/adt/ruleutils.c, not exported.
 *
 * Helper function to identify node types that satisfy func_expr_windowless.
 * If in doubt, "false" is always a safe answer.
 */
bool
looks_like_function(Node *node)
{
	if (node == NULL)
		return false;			/* probably shouldn't happen */
	switch (nodeTag(node))
	{
		case T_FuncExpr:
			/* OK, unless it's going to deparse as a cast */
			return (((FuncExpr *) node)->funcformat == COERCE_EXPLICIT_CALL);
		case T_NullIfExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_SQLValueFunction:
		case T_XmlExpr:
			/* these are all accepted by func_expr_common_subexpr */
			return true;
		default:
			break;
	}
	return false;
}

#if PG_VERSION_NUM >= 110000
/*
 * Copied from src/backend/catalog/partition.c, not exported
 *
 * qsort_partition_hbound_cmp
 *
 * We sort hash bounds by modulus, then by remainder.
 */
int32
qsort_partition_hbound_cmp(const void *a, const void *b)
{
	PartitionHashBound *h1 = (*(PartitionHashBound *const *) a);
	PartitionHashBound *h2 = (*(PartitionHashBound *const *) b);

	return partition_hbound_cmp(h1->modulus, h1->remainder,
								h2->modulus, h2->remainder);
}
#endif

/*
 * Copied from src/backend/catalog/partition.c, not exported
 *
 * qsort_partition_list_value_cmp
 *
 * Compare two list partition bound datums
 */
int32
qsort_partition_list_value_cmp(const void *a, const void *b, void *arg)
{
	Datum		val1 = (*(const PartitionListValue **) a)->value,
				val2 = (*(const PartitionListValue **) b)->value;
	PartitionKey key = (PartitionKey) arg;

	return DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[0],
										   key->partcollation[0],
										   val1, val2));
}

/*
 * Copied from src/backend/catalog/partition.c, not exported
 *
 * Used when sorting range bounds across all range partitions
 */
int32
qsort_partition_rbound_cmp(const void *a, const void *b, void *arg)
{
	PartitionRangeBound *b1 = (*(PartitionRangeBound *const *) a);
	PartitionRangeBound *b2 = (*(PartitionRangeBound *const *) b);
	PartitionKey key = (PartitionKey) arg;

#if PG_VERSION_NUM < 110000
	return partition_rbound_cmp(key, b1->datums, b1->kind, b1->lower, b2);
#else
	return partition_rbound_cmp(key->partnatts, key->partsupfunc,
								key->partcollation, b1->datums, b1->kind,
								b1->lower, b2);
#endif
}

/* ----------
 * Copied from src/backend/utils/adt/ruleutils.c, not exported.
 *
 * get_const_expr
 *
 *	Make a string representation of a Const
 *
 * showtype can be -1 to never show "::typename" decoration, or +1 to always
 * show it, or 0 to show it only if the constant wouldn't be assumed to be
 * the right type by default.
 *
 * If the Const's collation isn't default for its type, show that too.
 * We mustn't do this when showtype is -1 (since that means the caller will
 * print "::typename", and we can't put a COLLATE clause in between).  It's
 * caller's responsibility that collation isn't missed in such cases.
 * ----------
 */
void
get_const_expr(Const *constval, deparse_context *context, int showtype)
{
	StringInfo	buf = context->buf;
	Oid			typoutput;
	bool		typIsVarlena;
	char	   *extval;
	bool		needlabel = false;

	if (constval->constisnull)
	{
		/*
		 * Always label the type of a NULL constant to prevent misdecisions
		 * about type when reparsing.
		 */
		appendStringInfoString(buf, "NULL");
		if (showtype >= 0)
		{
			appendStringInfo(buf, "::%s",
							 format_type_with_typemod(constval->consttype,
													  constval->consttypmod));
			get_const_collation(constval, context);
		}
		return;
	}

	getTypeOutputInfo(constval->consttype,
					  &typoutput, &typIsVarlena);

	extval = OidOutputFunctionCall(typoutput, constval->constvalue);

	switch (constval->consttype)
	{
		case INT4OID:

			/*
			 * INT4 can be printed without any decoration, unless it is
			 * negative; in that case print it as '-nnn'::integer to ensure
			 * that the output will re-parse as a constant, not as a constant
			 * plus operator.  In most cases we could get away with printing
			 * (-nnn) instead, because of the way that gram.y handles negative
			 * literals; but that doesn't work for INT_MIN, and it doesn't
			 * seem that much prettier anyway.
			 */
			if (extval[0] != '-')
				appendStringInfoString(buf, extval);
			else
			{
				appendStringInfo(buf, "'%s'", extval);
				needlabel = true;	/* we must attach a cast */
			}
			break;

		case NUMERICOID:

			/*
			 * NUMERIC can be printed without quotes if it looks like a float
			 * constant (not an integer, and not Infinity or NaN) and doesn't
			 * have a leading sign (for the same reason as for INT4).
			 */
			if (isdigit((unsigned char) extval[0]) &&
				strcspn(extval, "eE.") != strlen(extval))
			{
				appendStringInfoString(buf, extval);
			}
			else
			{
				appendStringInfo(buf, "'%s'", extval);
				needlabel = true;	/* we must attach a cast */
			}
			break;

		case BITOID:
		case VARBITOID:
			appendStringInfo(buf, "B'%s'", extval);
			break;

		case BOOLOID:
			if (strcmp(extval, "t") == 0)
				appendStringInfoString(buf, "true");
			else
				appendStringInfoString(buf, "false");
			break;

		default:
			simple_quote_literal(buf, extval);
			break;
	}

	pfree(extval);

	if (showtype < 0)
		return;

	/*
	 * For showtype == 0, append ::typename unless the constant will be
	 * implicitly typed as the right type when it is read in.
	 *
	 * XXX this code has to be kept in sync with the behavior of the parser,
	 * especially make_const.
	 */
	switch (constval->consttype)
	{
		case BOOLOID:
		case UNKNOWNOID:
			/* These types can be left unlabeled */
			needlabel = false;
			break;
		case INT4OID:
			/* We determined above whether a label is needed */
			break;
		case NUMERICOID:

			/*
			 * Float-looking constants will be typed as numeric, which we
			 * checked above; but if there's a nondefault typmod we need to
			 * show it.
			 */
			needlabel |= (constval->consttypmod >= 0);
			break;
		default:
			needlabel = true;
			break;
	}
	if (needlabel || showtype > 0)
		appendStringInfo(buf, "::%s",
						 format_type_with_typemod(constval->consttype,
												  constval->consttypmod));

	get_const_collation(constval, context);
}

/*
 * Copied from src/backend/utils/adt/ruleutils.c, not exported.
 *
 * helper for get_const_expr: append COLLATE if needed
 */
void
get_const_collation(Const *constval, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (OidIsValid(constval->constcollid))
	{
		Oid			typcollation = get_typcollation(constval->consttype);

		if (constval->constcollid != typcollation)
		{
			appendStringInfo(buf, " COLLATE %s",
							 generate_collation_name(constval->constcollid));
		}
	}
}

/*
 * Copied from src/backend/utils/adt/ruleutils.c, not exported.
 *
 * simple_quote_literal - Format a string as a SQL literal, append to buf
 */
void
simple_quote_literal(StringInfo buf, const char *val)
{
	const char *valptr;

	/*
	 * We form the string literal according to the prevailing setting of
	 * standard_conforming_strings; we never use E''. User is responsible for
	 * making sure result is used correctly.
	 */
	appendStringInfoChar(buf, '\'');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		if (SQL_STR_DOUBLE(ch, !standard_conforming_strings))
			appendStringInfoChar(buf, ch);
		appendStringInfoChar(buf, ch);
	}
	appendStringInfoChar(buf, '\'');
}

/*
 * Copied from src/backend/parser/parse_utilcmd.c, not exported
 *
 * Transform one constant in a partition bound spec
 */
Const *
transformPartitionBoundValue(ParseState *pstate, A_Const *con,
		const char *colName, Oid colType, int32 colTypmod)
{
	Node	   *value;

	/* Make it into a Const */
	value = (Node *) make_const(pstate, &con->val, con->location);

	/* Coerce to correct type */
	value = coerce_to_target_type(pstate,
								  value, exprType(value),
								  colType,
								  colTypmod,
								  COERCION_ASSIGNMENT,
								  COERCE_IMPLICIT_CAST,
								  -1);

	if (value == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("specified value cannot be cast to type %s for column \"%s\"",
						format_type_be(colType), colName),
				 parser_errposition(pstate, con->location)));

	/* Simplify the expression, in case we had a coercion */
	if (!IsA(value, Const))
		value = (Node *) expression_planner((Expr *) value);

	/* Fail if we don't have a constant (i.e., non-immutable coercion) */
	if (!IsA(value, Const))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("specified value cannot be cast to type %s for column \"%s\"",
						format_type_be(colType), colName),
				 errdetail("The cast requires a non-immutable conversion."),
				 errhint("Try putting the literal value in single quotes."),
				 parser_errposition(pstate, con->location)));

	return (Const *) value;
}

/*
 * Copied from src/backend/parser/parse_utilcmd.c, not exported
 *
 * validateInfiniteBounds
 *
 * Check that a MAXVALUE or MINVALUE specification in a partition bound is
 * followed only by more of the same.
 */
void
validateInfiniteBounds(ParseState *pstate, List *blist)
{
	ListCell   *lc;
	PartitionRangeDatumKind kind = PARTITION_RANGE_DATUM_VALUE;

	foreach(lc, blist)
	{
		PartitionRangeDatum *prd = castNode(PartitionRangeDatum, lfirst(lc));

		if (kind == prd->kind)
			continue;

		switch (kind)
		{
			case PARTITION_RANGE_DATUM_VALUE:
				kind = prd->kind;
				break;

			case PARTITION_RANGE_DATUM_MAXVALUE:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("every bound following MAXVALUE must also be MAXVALUE"),
						 parser_errposition(pstate, exprLocation((Node *) prd))));

			case PARTITION_RANGE_DATUM_MINVALUE:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("every bound following MINVALUE must also be MINVALUE"),
						 parser_errposition(pstate, exprLocation((Node *) prd))));
		}
	}
}


/*
 * Imported from src/backend/catalog/partition.c, not exported
 *
 * get_partition_operator
 *
 * Return oid of the operator of given strategy for a given partition key
 * column.
 */
Oid
get_partition_operator(PartitionKey key, int col, StrategyNumber strategy,
					   bool *need_relabel)
{
	Oid			operoid;

	/*
	 * First check if there exists an operator of the given strategy, with
	 * this column's type as both its lefttype and righttype, in the
	 * partitioning operator family specified for the column.
	 */
	operoid = get_opfamily_member(key->partopfamily[col],
								  key->parttypid[col],
								  key->parttypid[col],
								  strategy);

	/*
	 * If one doesn't exist, we must resort to using an operator in the same
	 * operator family but with the operator class declared input type.  It is
	 * OK to do so, because the column's type is known to be binary-coercible
	 * with the operator class input type (otherwise, the operator class in
	 * question would not have been accepted as the partitioning operator
	 * class).  We must however inform the caller to wrap the non-Const
	 * expression with a RelabelType node to denote the implicit coercion. It
	 * ensures that the resulting expression structurally matches similarly
	 * processed expressions within the optimizer.
	 */
	if (!OidIsValid(operoid))
	{
		operoid = get_opfamily_member(key->partopfamily[col],
									  key->partopcintype[col],
									  key->partopcintype[col],
									  strategy);
		if (!OidIsValid(operoid))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 strategy, key->partopcintype[col], key->partopcintype[col],
				 key->partopfamily[col]);
		*need_relabel = true;
	}
	else
		*need_relabel = false;

	return operoid;
}


/*
 * Copied from src/backend/catalog/partition.c, not exported
 *
 * make_partition_op_expr
 *		Returns an Expr for the given partition key column with arg1 and
 *		arg2 as its leftop and rightop, respectively
 */
Expr *
make_partition_op_expr(PartitionKey key, int keynum,
		       uint16 strategy, Expr *arg1, Expr *arg2)
{
	Oid			operoid;
	bool		need_relabel = false;
	Expr	   *result = NULL;

	/* Get the correct btree operator for this partitioning column */
	operoid = get_partition_operator(key, keynum, strategy, &need_relabel);

	/*
	 * Chosen operator may be such that the non-Const operand needs to be
	 * coerced, so apply the same; see the comment in
	 * get_partition_operator().
	 */
	if (!IsA(arg1, Const) &&
			(need_relabel ||
			 key->partcollation[keynum] != key->parttypcoll[keynum]))
		arg1 = (Expr *) makeRelabelType(arg1,
				key->partopcintype[keynum],
				-1,
				key->partcollation[keynum],
				COERCE_EXPLICIT_CAST);

	/* Generate the actual expression */
	switch (key->strategy)
	{
		case PARTITION_STRATEGY_LIST:
			{
				List	   *elems = (List *) arg2;
				int			nelems = list_length(elems);

				Assert(nelems >= 1);
				Assert(keynum == 0);

				if (nelems > 1 &&
						!type_is_array(key->parttypid[keynum]))
				{
					ArrayExpr  *arrexpr;
					ScalarArrayOpExpr *saopexpr;

					/* Construct an ArrayExpr for the right-hand inputs */
					arrexpr = makeNode(ArrayExpr);
					arrexpr->array_typeid =
						get_array_type(key->parttypid[keynum]);
					arrexpr->array_collid = key->parttypcoll[keynum];
					arrexpr->element_typeid = key->parttypid[keynum];
					arrexpr->elements = elems;
					arrexpr->multidims = false;
					arrexpr->location = -1;

					/* Build leftop = ANY (rightop) */
					saopexpr = makeNode(ScalarArrayOpExpr);
					saopexpr->opno = operoid;
					saopexpr->opfuncid = get_opcode(operoid);
					saopexpr->useOr = true;
					saopexpr->inputcollid = key->partcollation[keynum];
					saopexpr->args = list_make2(arg1, arrexpr);
					saopexpr->location = -1;

					result = (Expr *) saopexpr;
				}
				else
				{
					List	   *elemops = NIL;
					ListCell   *lc;

					foreach (lc, elems)
					{
						Expr   *elem = lfirst(lc),
							   *elemop;

						elemop = make_opclause(operoid,
								BOOLOID,
								false,
								arg1, elem,
								InvalidOid,
								key->partcollation[keynum]);
						elemops = lappend(elemops, elemop);
					}

					result = nelems > 1 ? makeBoolExpr(OR_EXPR, elemops, -1) : linitial(elemops);
				}
				break;
			}

		case PARTITION_STRATEGY_RANGE:
			result = make_opclause(operoid,
					BOOLOID,
					false,
					arg1, arg2,
					InvalidOid,
					key->partcollation[keynum]);
			break;

		default:
			elog(ERROR, "invalid partitioning strategy");
			break;
	}

	return result;
}

/*
 * Copied from src/backend/catalog/partition.c, not exported
 *
 * get_range_key_properties
 *		Returns range partition key information for a given column
 *
 * This is a subroutine for get_qual_for_range, and its API is pretty
 * specialized to that caller.
 *
 * Constructs an Expr for the key column (returned in *keyCol) and Consts
 * for the lower and upper range limits (returned in *lower_val and
 * *upper_val).  For MINVALUE/MAXVALUE limits, NULL is returned instead of
 * a Const.  All of these structures are freshly palloc'd.
 *
 * *partexprs_item points to the cell containing the next expression in
 * the key->partexprs list, or NULL.  It may be advanced upon return.
 */
void
get_range_key_properties(PartitionKey key, int keynum,
			 PartitionRangeDatum *ldatum,
			 PartitionRangeDatum *udatum,
			 ListCell **partexprs_item,
			 Expr **keyCol,
			 Const **lower_val, Const **upper_val)
{
	/* Get partition key expression for this column */
	if (key->partattrs[keynum] != 0)
	{
		*keyCol = (Expr *) makeVar(1,
				key->partattrs[keynum],
				key->parttypid[keynum],
				key->parttypmod[keynum],
				key->parttypcoll[keynum],
				0);
	}
	else
	{
		if (*partexprs_item == NULL)
			elog(ERROR, "wrong number of partition key expressions");
		*keyCol = copyObject(lfirst(*partexprs_item));
		*partexprs_item = lnext(*partexprs_item);
	}

	/* Get appropriate Const nodes for the bounds */
	if (ldatum->kind == PARTITION_RANGE_DATUM_VALUE)
		*lower_val = castNode(Const, copyObject(ldatum->value));
	else
		*lower_val = NULL;

	if (udatum->kind == PARTITION_RANGE_DATUM_VALUE)
		*upper_val = castNode(Const, copyObject(udatum->value));
	else
		*upper_val = NULL;
}


/*
 * Copied from src/backend/catalog/partition.c, not exported
 *
 * get_range_nulltest
 *
 * A non-default range partition table does not currently allow partition
 * keys to be null, so emit an IS NOT NULL expression for each key column.
 */
List *
get_range_nulltest(PartitionKey key)
{
	List	   *result = NIL;
	NullTest   *nulltest;
	ListCell   *partexprs_item;
	int			i;

	partexprs_item = list_head(key->partexprs);
	for (i = 0; i < key->partnatts; i++)
	{
		Expr	   *keyCol;

		if (key->partattrs[i] != 0)
		{
			keyCol = (Expr *) makeVar(1,
					key->partattrs[i],
					key->parttypid[i],
					key->parttypmod[i],
					key->parttypcoll[i],
					0);
		}
		else
		{
			if (partexprs_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");
			keyCol = copyObject(lfirst(partexprs_item));
			partexprs_item = lnext(partexprs_item);
		}

		nulltest = makeNode(NullTest);
		nulltest->arg = keyCol;
		nulltest->nulltesttype = IS_NOT_NULL;
		nulltest->argisrow = false;
		nulltest->location = -1;
		result = lappend(result, nulltest);
	}

	return result;
}


/*
 * Copied from src/backend/optimizer/prep/prepunion.c, not exported
 *
 * make_inh_translation_list
 *	  Build the list of translations from parent Vars to child Vars for
 *	  an inheritance child.
 *
 * For paranoia's sake, we match type/collation as well as attribute name.
 */
void
make_inh_translation_list(Relation oldrelation, Relation newrelation,
						  Index newvarno,
						  List **translated_vars)
{
	List	   *vars = NIL;
	TupleDesc	old_tupdesc = RelationGetDescr(oldrelation);
	TupleDesc	new_tupdesc = RelationGetDescr(newrelation);
	int			oldnatts = old_tupdesc->natts;
	int			newnatts = new_tupdesc->natts;
	int			old_attno;

	for (old_attno = 0; old_attno < oldnatts; old_attno++)
	{
		Form_pg_attribute att;
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcollation;
		int			new_attno;

		att = TupleDescAttr(old_tupdesc, old_attno);
		if (att->attisdropped)
		{
			/* Just put NULL into this list entry */
			vars = lappend(vars, NULL);
			continue;
		}
		attname = NameStr(att->attname);
		atttypid = att->atttypid;
		atttypmod = att->atttypmod;
		attcollation = att->attcollation;

		/*
		 * When we are generating the "translation list" for the parent table
		 * of an inheritance set, no need to search for matches.
		 */
		if (oldrelation == newrelation)
		{
			vars = lappend(vars, makeVar(newvarno,
										 (AttrNumber) (old_attno + 1),
										 atttypid,
										 atttypmod,
										 attcollation,
										 0));
			continue;
		}

		/*
		 * Otherwise we have to search for the matching column by name.
		 * There's no guarantee it'll have the same column position, because
		 * of cases like ALTER TABLE ADD COLUMN and multiple inheritance.
		 * However, in simple cases it will be the same column number, so try
		 * that before we go groveling through all the columns.
		 *
		 * Note: the test for (att = ...) != NULL cannot fail, it's just a
		 * notational device to include the assignment into the if-clause.
		 */
		if (old_attno < newnatts &&
			(att = TupleDescAttr(new_tupdesc, old_attno)) != NULL &&
			!att->attisdropped && att->attinhcount != 0 &&
			strcmp(attname, NameStr(att->attname)) == 0)
			new_attno = old_attno;
		else
		{
			for (new_attno = 0; new_attno < newnatts; new_attno++)
			{
				att = TupleDescAttr(new_tupdesc, new_attno);
				if (!att->attisdropped && att->attinhcount != 0 &&
					strcmp(attname, NameStr(att->attname)) == 0)
					break;
			}
			if (new_attno >= newnatts)
				elog(ERROR, "could not find inherited attribute \"%s\" of relation \"%s\"",
					 attname, RelationGetRelationName(newrelation));
		}

		/* Found it, check type and collation match */
		if (atttypid != att->atttypid || atttypmod != att->atttypmod)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's type",
				 attname, RelationGetRelationName(newrelation));
		if (attcollation != att->attcollation)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's collation",
				 attname, RelationGetRelationName(newrelation));

		vars = lappend(vars, makeVar(newvarno,
									 (AttrNumber) (new_attno + 1),
									 atttypid,
									 atttypmod,
									 attcollation,
									 0));
	}

	*translated_vars = vars;
}

#endif			/* pg10+ */
