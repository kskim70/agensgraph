/*
 * nodeModifyGraph.c
 *	  routines to handle ModifyGraph nodes.
 *
 * Copyright (c) 2016 by Bitnine Global, Inc.
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeModifyGraph.c
 */

#include "postgres.h"

#include "ag_const.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/ag_graph_fn.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeModifyGraph.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/graphnodes.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/graph.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

#define SQLCMD_BUFLEN				(NAMEDATALEN + 192)

/*
 * NOTE: If you add SQLCMD, you should add SqlcmdType for it and use
 *       sqlcmd_cache to run the SQLCMD.
 */
#define SQLCMD_DEL_ELEM \
	"DELETE FROM ONLY \"%s\".\"%s\" WHERE " AG_ELEM_LOCAL_ID " = $1"
#define SQLCMD_DEL_ELEM_NPARAMS		1

#define SQLCMD_DETACH \
	"SELECT " AG_ELEM_LOCAL_ID " FROM \"%s\"." AG_EDGE \
	" WHERE " AG_START_ID " = $1 OR \"" AG_END_ID "\" = $1"
#define SQLCMD_DETACH_NPARAMS		1

#define SQLCMD_DEL_EDGES \
	"DELETE FROM \"%s\"." AG_EDGE \
	" WHERE " AG_START_ID " = $1 OR \"" AG_END_ID "\" = $1"
#define SQLCMD_DEL_EDGES_NPARAMS	1

#define SQLCMD_SET_PROP \
	"UPDATE \"%s\".\"%s\" SET properties = $1 WHERE " AG_ELEM_LOCAL_ID " = $2"
#define SQLCMD_SET_PROP_NPARAMS		2

#define DATUM_NULL	PointerGetDatum(NULL)

typedef enum SqlcmdType
{
	SQLCMD_TYPE_DEL_ELEM,
	SQLCMD_TYPE_DETACH,
	SQLCMD_TYPE_DEL_EDGES,
	SQLCMD_TYPE_SET_PROP,
} SqlcmdType;

typedef struct ArrayAccessTypeInfo
{
	int16		typlen;
	bool		typbyval;
	char		typalign;
} ArrayAccessTypeInfo;

typedef enum DelElemKind
{
	DEL_ELEM_VERTEX,
	DEL_ELEM_EDGE
} DelElemKind;

/* hash key */
typedef struct SqlcmdKey
{
	SqlcmdType	cmdtype;
	uint16		labid;
} SqlcmdKey;

/* hash entry */
typedef struct SqlcmdEntry
{
	SqlcmdKey	key;
	SPIPlanPtr	plan;
} SqlcmdEntry;

/* hash entry */
typedef struct ModifiedPropEntry
{
	Graphid		key;
	union
	{
		Datum		properties;
		DelElemKind	kind;
	}			val;
} ModifiedPropEntry;

static HTAB *sqlcmd_cache = NULL;

static void initGraphWRStats(ModifyGraphState *mgstate, GraphWriteOp op);
static List *ExecInitGraphPattern(List *pattern, ModifyGraphState *mgstate);
static List *ExecInitGraphSets(List *sets, ModifyGraphState *mgstate);
static TupleTableSlot *ExecCreateGraph(ModifyGraphState *mgstate,
									   TupleTableSlot *slot);
static TupleTableSlot *createPath(ModifyGraphState *mgstate, GraphPath *path,
								  TupleTableSlot *slot);
static Datum createVertex(ModifyGraphState *mgstate, GraphVertex *gvertex,
						  Graphid *vid, TupleTableSlot *slot, bool inPath);
static Datum createEdge(ModifyGraphState *mgstate, GraphEdge *gedge,
						Graphid start, Graphid end, TupleTableSlot *slot,
						bool inPath);
static ResultRelInfo *getResultRelInfo(ModifyGraphState *mgstate, Oid relid);
static Datum findVertex(TupleTableSlot *slot, GraphVertex *node, Graphid *vid);
static Datum findEdge(TupleTableSlot *slot, GraphEdge *node, Graphid *eid);
static AttrNumber findAttrInSlotByName(TupleTableSlot *slot, char *name);
static void setSlotValueByName(TupleTableSlot *slot, Datum value, char *name);
static void setSlotValueByAttnum(TupleTableSlot *slot, Datum value, int attnum);
static Datum *makeDatumArray(ExprContext *econtext, int len);
static TupleTableSlot *ExecDeleteGraph(ModifyGraphState *mgstate,
									   TupleTableSlot *slot);
static void deleteVertex(ModifyGraphState *mgstate, Datum vertex, bool detach);
static bool vertexHasEdge(ModifyGraphState *mgstate, Datum vid);
static void deleteVertexEdges(ModifyGraphState *mgstate, Datum vid);
static void deleteElem(ModifyGraphState *mgstate, Datum id, DelElemKind kind);
static void deletePath(ModifyGraphState *mgstate, Datum graphpath, bool detach);
static TupleTableSlot *ExecSetGraph(ModifyGraphState *mgstate, GSPKind kind,
									TupleTableSlot *slot);
static void updateElemProp(ModifyGraphState *mgstate, Datum id, Datum expr);
static Datum makeModifiedElem(Datum elem, Oid elemtype, Datum id,
							  Datum prop_map);
static TupleTableSlot *ExecMergeGraph(ModifyGraphState *mgstate,
									  TupleTableSlot *slot);
static bool isMatchedMergePattern(PlanState *planstate);
static TupleTableSlot *createMergePath(ModifyGraphState *mgstate,
									   GraphPath *path, TupleTableSlot *slot);
static Datum createMergeVertex(ModifyGraphState *mgstate,
							   GraphVertex *gvertex,
							   Graphid *vid, TupleTableSlot *slot);
static Datum createMergeEdge(ModifyGraphState *mgstate, GraphEdge *gedge,
							 Graphid start, Graphid end, TupleTableSlot *slot);
static TupleTableSlot *copyVirtualTupleTableSlot(TupleTableSlot *dstslot,
												 TupleTableSlot *srcslot);

/* caching SPIPlan's (See ri_triggers.c) */
static void InitSqlcmdHashTable(MemoryContext mcxt);
static void EndSqlcmdHashTable(void);
static SPIPlanPtr findPreparedPlan(SqlcmdKey *key);
static SPIPlanPtr prepareSqlcmd(SqlcmdKey *key, char *sqlcmd,
								int nargs, Oid *argtypes);
static void savePreparedPlan(SqlcmdKey *key, SPIPlanPtr plan);

/* eager */
static void enterSetPropTable(ModifyGraphState *mgstate, Datum elem,
							  Datum prop);
static void enterDelPropTable(ModifyGraphState *mgstate, Datum elem, Oid type);
static void getGidListInPath(Datum graphpath, List **vtxlist, List **edgelist);
static Datum getVertexFinalPropMap(ModifyGraphState *mgstate,
								   Datum origin, Graphid gid);
static Datum getEdgeFinalPropMap(ModifyGraphState *mgstate,
								 Datum origin, Graphid gid);
static Datum getPathFinalPropMap(ModifyGraphState *node, Datum origin);
static void reflectModifiedProp(ModifyGraphState *mgstate);

ModifyGraphState *
ExecInitModifyGraph(ModifyGraph *mgplan, EState *estate, int eflags)
{
	ModifyGraphState *mgstate;

	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	mgstate = makeNode(ModifyGraphState);
	mgstate->ps.plan = (Plan *) mgplan;
	mgstate->ps.state = estate;

	/* Tuple desc for result is the same as the subplan. */
	ExecInitResultTupleSlot(estate, &mgstate->ps);
	ExecAssignResultType(&mgstate->ps,
						 ExecTypeFromTL(mgplan->subplan->targetlist, false));

	ExecAssignExprContext(estate, &mgstate->ps);

	mgstate->canSetTag = mgplan->canSetTag;
	mgstate->done = false;
	mgstate->child_done = false;
	mgstate->eagerness = mgplan->eagerness;
	mgstate->subplan = ExecInitNode(mgplan->subplan, estate, eflags);
	Assert(mgplan->operation != GWROP_MERGE ||
		   IsA(mgstate->subplan, NestLoopState));

	mgstate->elemTupleSlot = ExecInitExtraTupleSlot(estate);

	mgstate->graphid = get_graph_path_oid();
	mgstate->graphname = get_graph_path(false);
	mgstate->edgeid = get_labname_labid(AG_EDGE, mgstate->graphid);
	mgstate->numOldRtable = list_length(estate->es_range_table);

	mgstate->pattern = ExecInitGraphPattern(mgplan->pattern, mgstate);

	if (mgplan->targets != NIL)
	{
		int			numResultRelInfo = list_length(mgplan->targets);
		ResultRelInfo *resultRelInfos;
		ParseState *pstate;
		ResultRelInfo *resultRelInfo;
		ListCell   *lt;

		resultRelInfos = palloc(numResultRelInfo * sizeof(*resultRelInfos));

		pstate = make_parsestate(NULL);
		resultRelInfo = resultRelInfos;
		foreach(lt, mgplan->targets)
		{
			Oid			relid = lfirst_oid(lt);
			Relation	relation;
			RangeTblEntry *rte;

			relation = heap_open(relid, RowExclusiveLock);

			rte = addRangeTableEntryForRelation(pstate, relation,
												NULL, false, false);
			rte->requiredPerms = ACL_INSERT;
			estate->es_range_table = lappend(estate->es_range_table, rte);

			InitResultRelInfo(resultRelInfo,
							  relation,
							  list_length(estate->es_range_table),
							  estate->es_instrument);

			ExecOpenIndices(resultRelInfo, false);

			resultRelInfo++;
		}

		mgstate->resultRelations = resultRelInfos;
		mgstate->numResultRelations = numResultRelInfo;

		/* es_result_relation_info is NULL except ModifyTable case */
		estate->es_result_relation_info = NULL;

		free_parsestate(pstate);
	}

	mgstate->exprs = (List *) ExecInitExpr((Expr *) mgplan->exprs,
										   (PlanState *) mgstate);
	mgstate->sets = ExecInitGraphSets(mgplan->sets, mgstate);

	initGraphWRStats(mgstate, mgplan->operation);

	InitSqlcmdHashTable(estate->es_query_cxt);

	if (mgstate->eagerness && (mgstate->sets != NIL || mgstate->exprs != NIL))
	{
		HASHCTL ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Graphid);
		ctl.entrysize = sizeof(ModifiedPropEntry);
		ctl.hcxt = CurrentMemoryContext;

		mgstate->propTable = hash_create("modified object table", 128, &ctl,
										HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	else
	{
		mgstate->propTable = NULL;
	}

	mgstate->tuplestorestate = tuplestore_begin_heap(false, false, eager_mem);

	return mgstate;
}

TupleTableSlot *
ExecModifyGraph(ModifyGraphState *mgstate)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	EState	   *estate = mgstate->ps.state;

	if (mgstate->done)
		return NULL;

	if (!mgstate->child_done)
	{
		for (;;)
		{
			TupleTableSlot *slot;

			/* ExecInsertIndexTuples() uses per-tuple context. Reset it here. */
			ResetPerTupleExprContext(estate);

			slot = ExecProcNode(mgstate->subplan);
			if (TupIsNull(slot))
				break;

			DisableGraphDML = false;
			switch (plan->operation)
			{
				case GWROP_CREATE:
					slot = ExecCreateGraph(mgstate, slot);
					break;
				case GWROP_DELETE:
					slot = ExecDeleteGraph(mgstate, slot);
					break;
				case GWROP_SET:
					{
						ExprContext *econtext = mgstate->ps.ps_ExprContext;

						ResetExprContext(econtext);
						econtext->ecxt_scantuple = slot;

						slot = ExecSetGraph(mgstate, GSP_NORMAL, slot);
					}
					break;
				case GWROP_MERGE:
					slot = ExecMergeGraph(mgstate, slot);
					break;
				default:
					elog(ERROR, "unknown operation");
					break;
			}
			DisableGraphDML = true;

			if (mgstate->eagerness)
			{
				Assert(slot != NULL);

				tuplestore_puttupleslot(mgstate->tuplestorestate, slot);
			}
			else if (slot != NULL)
			{
				return slot;
			}
			else
			{
				Assert(plan->last == true);
			}
		}

		mgstate->child_done = true;

		if (mgstate->propTable != NULL &&
			hash_get_num_entries(mgstate->propTable) > 0)
			reflectModifiedProp(mgstate);
	}

	if (mgstate->eagerness)
	{
		TupleTableSlot *result;
		int			natts;
		int			i;

		/* don't care about scan direction */
		result = mgstate->ps.ps_ResultTupleSlot;
		tuplestore_gettupleslot(mgstate->tuplestorestate, true, false, result);

		if (TupIsNull(result))
			return result;

		slot_getallattrs(result);

		if (mgstate->propTable == NULL ||
			hash_get_num_entries(mgstate->propTable) < 1)
			return result;

		natts = result->tts_tupleDescriptor->natts;
		for (i = 0; i < natts; i++)
		{
			Oid			type;
			Graphid		gid;
			Datum		elem;

			if (result->tts_isnull[i])
				continue;

			type = result->tts_tupleDescriptor->attrs[i]->atttypid;
			if (type == VERTEXOID)
			{
				gid = getVertexIdDatum(result->tts_values[i]);
				elem = getVertexFinalPropMap(mgstate, result->tts_values[i],
											 gid);

				setSlotValueByAttnum(result, elem, i + 1);
			}
			else if (type == EDGEOID)
			{
				gid = getEdgeIdDatum(result->tts_values[i]);
				elem = getEdgeFinalPropMap(mgstate, result->tts_values[i], gid);

			}
			else if (type == GRAPHPATHOID)
			{
				elem = getPathFinalPropMap(mgstate, result->tts_values[i]);
			}
			else
			{
				elog(ERROR, "Invalid graph element type %d.", type);
			}

			setSlotValueByAttnum(result, elem, i + 1);
		}

		return result;
	}

	mgstate->done = true;

	return NULL;
}

void
ExecEndModifyGraph(ModifyGraphState *mgstate)
{
	EState	   *estate = mgstate->ps.state;
	ResultRelInfo *resultRelInfo;
	int			i;

	if (mgstate->tuplestorestate != NULL)
		tuplestore_end(mgstate->tuplestorestate);
	mgstate->tuplestorestate = NULL;

	if (mgstate->propTable != NULL)
		hash_destroy(mgstate->propTable);

	if (sqlcmd_cache != NULL)
		EndSqlcmdHashTable();

	resultRelInfo = mgstate->resultRelations;
	for (i = mgstate->numResultRelations; i > 0; i--)
	{
		ExecCloseIndices(resultRelInfo);
		heap_close(resultRelInfo->ri_RelationDesc, NoLock);

		resultRelInfo++;
	}

	/*
	 * PlannedStmt can be used as a cached plan,
	 * so remove the rtables added to this run.
	 */
	list_truncate(estate->es_range_table, mgstate->numOldRtable);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(mgstate->ps.ps_ResultTupleSlot);

	ExecEndNode(mgstate->subplan);
	ExecFreeExprContext(&mgstate->ps);
}

static void
initGraphWRStats(ModifyGraphState *mgstate, GraphWriteOp op)
{
	EState *estate = mgstate->ps.state;

	if (mgstate->pattern != NIL)
	{
		Assert(op == GWROP_CREATE || op == GWROP_MERGE);

		estate->es_graphwrstats.insertVertex = 0;
		estate->es_graphwrstats.insertEdge = 0;
	}
	if (mgstate->exprs != NIL)
	{
		Assert(op == GWROP_DELETE);

		estate->es_graphwrstats.deleteVertex = 0;
		estate->es_graphwrstats.deleteEdge = 0;
	}
	if (mgstate->sets != NIL)
	{
		Assert(op == GWROP_SET || op == GWROP_MERGE);

		estate->es_graphwrstats.updateProperty = 0;
	}
}

static List *
ExecInitGraphPattern(List *pattern, ModifyGraphState *mgstate)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	GraphPath  *gpath;
	ListCell   *le;

	if (plan->operation != GWROP_MERGE)
		return pattern;

	AssertArg(list_length(pattern) == 1);

	gpath = linitial(pattern);

	foreach(le, gpath->chain)
	{
		Node *elem = lfirst(le);

		if (IsA(elem, GraphVertex))
		{
			GraphVertex *gvertex = (GraphVertex *) elem;

			gvertex->es_expr = ExecInitExpr((Expr *) gvertex->expr,
											(PlanState *) mgstate);
			gvertex->es_qual = ExecInitExpr((Expr *) gvertex->qual,
											(PlanState *) mgstate);
		}
		else
		{
			GraphEdge *gedge = (GraphEdge *) elem;

			Assert(IsA(elem, GraphEdge));

			gedge->es_expr = ExecInitExpr((Expr *) gedge->expr,
										  (PlanState *) mgstate);
			gedge->es_qual = ExecInitExpr((Expr *) gedge->qual,
										  (PlanState *) mgstate);
		}
	}

	return pattern;
}

static TupleTableSlot *
ExecCreateGraph(ModifyGraphState *mgstate, TupleTableSlot *slot)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	ExprContext *econtext = mgstate->ps.ps_ExprContext;
	ListCell   *lp;

	ResetExprContext(econtext);

	/* create a pattern, accumulated paths `slot` has */
	foreach(lp, plan->pattern)
	{
		GraphPath *path = (GraphPath *) lfirst(lp);

		slot = createPath(mgstate, path, slot);
	}

	return (plan->last ? NULL : slot);
}

static List *
ExecInitGraphSets(List *sets, ModifyGraphState *mgstate)
{
	ListCell *ls;

	foreach(ls, sets)
	{
		GraphSetProp *gsp = lfirst(ls);

		gsp->es_elem = ExecInitExpr((Expr *) gsp->elem, (PlanState *) mgstate);
		gsp->es_expr = ExecInitExpr((Expr *) gsp->expr, (PlanState *) mgstate);
	}

	return sets;
}

/* create a path and accumulate it to the given slot */
static TupleTableSlot *
createPath(ModifyGraphState *mgstate, GraphPath *path, TupleTableSlot *slot)
{
	ExprContext *econtext = mgstate->ps.ps_ExprContext;
	bool		out = (path->variable != NULL);
	int			pathlen;
	Datum	   *vertices = NULL;
	Datum	   *edges = NULL;
	int			nvertices;
	int			nedges;
	ListCell   *le;
	Graphid		prevvid = 0;
	GraphEdge  *gedge = NULL;

	if (out)
	{
		pathlen = list_length(path->chain);
		Assert(pathlen % 2 == 1);

		vertices = makeDatumArray(econtext, (pathlen / 2) + 1);
		edges = makeDatumArray(econtext, pathlen / 2);

		nvertices = 0;
		nedges = 0;
	}

	foreach(le, path->chain)
	{
		Node *elem = (Node *) lfirst(le);

		if (IsA(elem, GraphVertex))
		{
			GraphVertex *gvertex = (GraphVertex *) elem;
			Graphid		vid;
			Datum		vertex;

			if (gvertex->create)
				vertex = createVertex(mgstate, gvertex, &vid, slot, out);
			else
				vertex = findVertex(slot, gvertex, &vid);

			Assert(vertex != DATUM_NULL);

			if (out)
				vertices[nvertices++] = vertex;

			if (gedge != NULL)
			{
				Datum edge;

				if (gedge->direction == GRAPH_EDGE_DIR_LEFT)
				{
					edge = createEdge(mgstate, gedge, vid, prevvid, slot, out);
				}
				else
				{
					Assert(gedge->direction == GRAPH_EDGE_DIR_RIGHT);

					edge = createEdge(mgstate, gedge, prevvid, vid, slot, out);
				}

				if (out)
					edges[nedges++] = edge;
			}

			prevvid = vid;
		}
		else
		{
			Assert(IsA(elem, GraphEdge));

			gedge = (GraphEdge *) elem;
		}
	}

	/* make a graphpath and set it to the slot */
	if (out)
	{
		MemoryContext oldmctx;
		Datum graphpath;

		Assert(nvertices == nedges + 1);
		Assert(pathlen == nvertices + nedges);

		oldmctx = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		graphpath = makeGraphpathDatum(vertices, nvertices, edges, nedges);

		MemoryContextSwitchTo(oldmctx);

		setSlotValueByName(slot, graphpath, path->variable);
	}

	return slot;
}

/*
 * createVertex - creates a vertex of a given node
 *
 * NOTE: This function returns a vertex if it must be in the result(`slot`).
 */
static Datum
createVertex(ModifyGraphState *mgstate, GraphVertex *gvertex, Graphid *vid,
			 TupleTableSlot *slot, bool inPath)
{
	EState	   *estate = mgstate->ps.state;
	TupleTableSlot *elemTupleSlot = mgstate->elemTupleSlot;
	ResultRelInfo *resultRelInfo;
	ResultRelInfo *savedResultRelInfo;
	Datum		vertex;
	Datum		vertexProp;
	HeapTuple	tuple;

	resultRelInfo = getResultRelInfo(mgstate, gvertex->relid);
	savedResultRelInfo = estate->es_result_relation_info;
	estate->es_result_relation_info = resultRelInfo;

	vertex = findVertex(slot, gvertex, vid);

	vertexProp = getVertexPropDatum(vertex);
	if (!JB_ROOT_IS_OBJECT(DatumGetJsonb(vertexProp)))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("jsonb object is expected for property map")));

	ExecClearTuple(elemTupleSlot);

	ExecSetSlotDescriptor(elemTupleSlot,
						  RelationGetDescr(resultRelInfo->ri_RelationDesc));
	elemTupleSlot->tts_values[0] = GraphidGetDatum(*vid);
	elemTupleSlot->tts_values[1] = vertexProp;
	MemSet(elemTupleSlot->tts_isnull, false,
		   elemTupleSlot->tts_tupleDescriptor->natts * sizeof(bool));
	ExecStoreVirtualTuple(elemTupleSlot);

	tuple = ExecMaterializeSlot(elemTupleSlot);

	/*
	 * Constraints might reference the tableoid column, so initialize
	 * t_tableOid before evaluating them.
	 */
	tuple->t_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	/*
	 * Check the constraints of the tuple
	 */
	if (resultRelInfo->ri_RelationDesc->rd_att->constr != NULL)
		ExecConstraints(resultRelInfo, elemTupleSlot, estate);

	/*
	 * insert the tuple normally
	 *
	 * NOTE: heap_insert() returns the cid of the new tuple in the t_self.
	 */
	heap_insert(resultRelInfo->ri_RelationDesc, tuple, estate->es_output_cid,
				0, NULL);

	/* insert index entries for the tuple */
	if (resultRelInfo->ri_NumIndices > 0)
		ExecInsertIndexTuples(elemTupleSlot, &(tuple->t_self), estate, false,
							  NULL, NIL);

	if (mgstate->canSetTag)
	{
		Assert(estate->es_graphwrstats.insertVertex != UINT_MAX);

		estate->es_graphwrstats.insertVertex++;
	}

	estate->es_result_relation_info = savedResultRelInfo;

	return vertex;
}

static Datum
createEdge(ModifyGraphState *mgstate, GraphEdge *gedge, Graphid start,
		   Graphid end, TupleTableSlot *slot, bool inPath)
{
	EState	   *estate = mgstate->ps.state;
	TupleTableSlot *elemTupleSlot = mgstate->elemTupleSlot;
	ResultRelInfo *resultRelInfo;
	ResultRelInfo *savedResultRelInfo;
	Graphid		id = 0;
	Datum		edge;
	Datum		edgeProp;
	HeapTuple	tuple;

	resultRelInfo = getResultRelInfo(mgstate, gedge->relid);
	savedResultRelInfo = estate->es_result_relation_info;
	estate->es_result_relation_info = resultRelInfo;

	edge = findEdge(slot, gedge, &id);
	Assert(edge != DATUM_NULL);

	edgeProp = getEdgePropDatum(edge);
	if (!JB_ROOT_IS_OBJECT(DatumGetJsonb(edgeProp)))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("jsonb object is expected for property map")));

	ExecClearTuple(elemTupleSlot);

	ExecSetSlotDescriptor(elemTupleSlot,
						  RelationGetDescr(resultRelInfo->ri_RelationDesc));
	elemTupleSlot->tts_values[0] = GraphidGetDatum(id);
	elemTupleSlot->tts_values[1] = GraphidGetDatum(start);
	elemTupleSlot->tts_values[2] = GraphidGetDatum(end);
	elemTupleSlot->tts_values[3] = edgeProp;
	MemSet(elemTupleSlot->tts_isnull, false,
		   elemTupleSlot->tts_tupleDescriptor->natts * sizeof(bool));
	ExecStoreVirtualTuple(elemTupleSlot);

	tuple = ExecMaterializeSlot(elemTupleSlot);

	tuple->t_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	if (resultRelInfo->ri_RelationDesc->rd_att->constr != NULL)
		ExecConstraints(resultRelInfo, elemTupleSlot, estate);

	heap_insert(resultRelInfo->ri_RelationDesc, tuple, estate->es_output_cid,
				0, NULL);

	if (resultRelInfo->ri_NumIndices > 0)
		ExecInsertIndexTuples(elemTupleSlot, &(tuple->t_self), estate, false,
							  NULL, NIL);

	edge = makeGraphEdgeDatum(elemTupleSlot->tts_values[0],
							  elemTupleSlot->tts_values[1],
							  elemTupleSlot->tts_values[2],
							  elemTupleSlot->tts_values[3]);

	if (gedge->resno > 0)
		setSlotValueByAttnum(slot, edge, gedge->resno);

	if (mgstate->canSetTag)
	{
		Assert(estate->es_graphwrstats.insertEdge != UINT_MAX);

		estate->es_graphwrstats.insertEdge++;
	}

	estate->es_result_relation_info = savedResultRelInfo;

	return edge;
}

static ResultRelInfo *
getResultRelInfo(ModifyGraphState *mgstate, Oid relid)
{
	ResultRelInfo *resultRelInfo;
	int			i;

	resultRelInfo = mgstate->resultRelations;
	for (i = 0; i < mgstate->numResultRelations; i++)
	{
		if (RelationGetRelid(resultRelInfo->ri_RelationDesc) == relid)
			break;

		resultRelInfo++;
	}

	if (i >= mgstate->numResultRelations)
		elog(ERROR, "invalid object ID %u for the target label", relid);

	return resultRelInfo;
}

static Datum
findVertex(TupleTableSlot *slot, GraphVertex *gvertex, Graphid *vid)
{
	bool		isnull;
	Datum		vertex;

	if (gvertex->resno == InvalidAttrNumber)
		return DATUM_NULL;

	vertex = slot_getattr(slot, gvertex->resno, &isnull);
	if (isnull)
		return DATUM_NULL;

	if (vid != NULL)
		*vid = DatumGetGraphid(getVertexIdDatum(vertex));

	return vertex;
}

static Datum
findEdge(TupleTableSlot *slot, GraphEdge *gedge, Graphid *eid)
{
	bool		isnull;
	Datum		edge;

	if (gedge->resno == InvalidAttrNumber)
		return DATUM_NULL;

	edge = slot_getattr(slot, gedge->resno, &isnull);
	if (isnull)
		return DATUM_NULL;

	if (eid != NULL)
		*eid = DatumGetGraphid(getEdgeIdDatum(edge));

	return edge;
}

static AttrNumber
findAttrInSlotByName(TupleTableSlot *slot, char *name)
{
	TupleDesc	tupDesc = slot->tts_tupleDescriptor;
	int			i;

	for (i = 0; i < tupDesc->natts; i++)
	{
		if (namestrcmp(&(tupDesc->attrs[i]->attname), name) == 0 &&
			!tupDesc->attrs[i]->attisdropped)
			return tupDesc->attrs[i]->attnum;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_NAME),
			 errmsg("variable \"%s\" does not exist", name)));
	return InvalidAttrNumber;
}

static void
setSlotValueByName(TupleTableSlot *slot, Datum value, char *name)
{
	AttrNumber attno;

	attno = findAttrInSlotByName(slot, name);

	slot->tts_values[attno - 1] = value;
	slot->tts_isnull[attno - 1] = false;
}

static void
setSlotValueByAttnum(TupleTableSlot *slot, Datum value, int attnum)
{
	AssertArg(attnum > 0 && attnum <= slot->tts_tupleDescriptor->natts);

	slot->tts_values[attnum - 1] = value;
	slot->tts_isnull[attnum - 1] = (value == (Datum) NULL) ? true : false;
}

static Datum *
makeDatumArray(ExprContext *econtext, int len)
{
	MemoryContext oldmctx;
	Datum *result;

	if (len == 0)
		return NULL;

	oldmctx = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	result = palloc(len * sizeof(Datum));

	MemoryContextSwitchTo(oldmctx);

	return result;
}

static TupleTableSlot *
ExecDeleteGraph(ModifyGraphState *mgstate, TupleTableSlot *slot)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	ExprContext *econtext = mgstate->ps.ps_ExprContext;
	ListCell   *le;

	ResetExprContext(econtext);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	foreach(le, mgstate->exprs)
	{
		ExprState  *e = (ExprState *) lfirst(le);
		Oid			type;
		Datum		datum;
		bool		isNull;
		ExprDoneCond isDone;

		type = exprType((Node *) e->expr);
		if (!(type == VERTEXOID || type == EDGEOID || type == GRAPHPATHOID))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("expected node, relationship, or path")));

		econtext->ecxt_scantuple = slot;
		datum = ExecEvalExpr(e, econtext, &isNull, &isDone);
		if (isNull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("deleting NULL is not allowed")));
		if (isDone != ExprSingleResult)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("expected single result")));

		if (mgstate->eagerness)
		{
			if (type == VERTEXOID && !plan->detach)
			{
				Datum id_datum = getVertexIdDatum(datum);

				if (vertexHasEdge(mgstate, id_datum))
				{
					Graphid		id = DatumGetGraphid(id_datum);
					Oid			relid = get_labid_relid(mgstate->graphid,
														GraphidGetLabid(id));

					ereport(ERROR,
							(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
							 errmsg("vertex " INT64_FORMAT
									" in \"%s\" has edge(s)",
									GraphidGetLocid(id),
									get_rel_name(relid))));
				}
			}

			enterDelPropTable(mgstate, datum, type);
		}
		else
		{
			switch (type)
			{
				case VERTEXOID:
					deleteVertex(mgstate, datum, plan->detach);
					break;
				case EDGEOID:
					deleteElem(mgstate, getEdgeIdDatum(datum), DEL_ELEM_EDGE);
					break;
				case GRAPHPATHOID:
					deletePath(mgstate, datum, plan->detach);
					break;
				default:
					elog(ERROR, "expected node, relationship, or path");
			}
		}
	}

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return (plan->last ? NULL : slot);
}

static void
deleteVertex(ModifyGraphState *mgstate, Datum vertex, bool detach)
{
	Datum id_datum = getVertexIdDatum(vertex);

	if (detach)
	{
		deleteVertexEdges(mgstate, id_datum);
	}
	else if (vertexHasEdge(mgstate, id_datum))
	{
		Graphid		id = DatumGetGraphid(id_datum);
		Oid			relid = get_labid_relid(mgstate->graphid,
											GraphidGetLabid(id));

		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("vertex " INT64_FORMAT " in \"%s\" has edge(s)",
						GraphidGetLocid(id), get_rel_name(relid))));
	}

	deleteElem(mgstate, id_datum, DEL_ELEM_VERTEX);
}

static bool
vertexHasEdge(ModifyGraphState *mgstate, Datum vid)
{
	Datum		values[SQLCMD_DETACH_NPARAMS];
	Oid			argTypes[SQLCMD_DETACH_NPARAMS] = {GRAPHIDOID};
	SqlcmdKey	key;
	SPIPlanPtr	plan;
	int			ret;

	key.cmdtype = SQLCMD_TYPE_DETACH;
	key.labid = mgstate->edgeid;
	plan = findPreparedPlan(&key);
	if (plan == NULL)
	{
		char sqlcmd[SQLCMD_BUFLEN];

		snprintf(sqlcmd, SQLCMD_BUFLEN, SQLCMD_DETACH, mgstate->graphname);

		plan = prepareSqlcmd(&key, sqlcmd, SQLCMD_DETACH_NPARAMS, argTypes);
	}

	values[0] = vid;

	ret = SPI_execp(plan, values, NULL, 0);
	if (ret != SPI_OK_SELECT)
	{
		Graphid id = DatumGetGraphid(vid);

		elog(ERROR, "DETACH (%hu." INT64_FORMAT "): SPI_execp returned %d",
			 GraphidGetLabid(id), GraphidGetLocid(id), ret);
	}

	return (SPI_processed > 0);
}

static void
deleteVertexEdges(ModifyGraphState *mgstate, Datum vid)
{
	EState	   *estate = mgstate->ps.state;
	Datum		values[SQLCMD_DEL_EDGES_NPARAMS];
	Oid			argTypes[SQLCMD_DEL_EDGES_NPARAMS] = {GRAPHIDOID};
	SqlcmdKey	key;
	SPIPlanPtr	plan;
	int			ret;

	key.cmdtype = SQLCMD_TYPE_DEL_EDGES;
	key.labid = mgstate->edgeid;
	plan = findPreparedPlan(&key);
	if (plan == NULL)
	{
		char sqlcmd[SQLCMD_BUFLEN];

		snprintf(sqlcmd, SQLCMD_BUFLEN, SQLCMD_DEL_EDGES, mgstate->graphname);

		plan = prepareSqlcmd(&key, sqlcmd, SQLCMD_DEL_EDGES_NPARAMS, argTypes);
	}

	values[0] = vid;

	ret = SPI_execp(plan, values, NULL, 0);
	if (ret != SPI_OK_DELETE)
	{
		Graphid id = DatumGetGraphid(vid);

		elog(ERROR, "DEL_EDGES (%hu." INT64_FORMAT "): SPI_execp returned %d",
			 GraphidGetLabid(id), GraphidGetLocid(id), ret);
	}

	if (mgstate->canSetTag)
	{
		Assert(estate->es_graphwrstats.deleteEdge != UINT_MAX);

		estate->es_graphwrstats.deleteEdge += SPI_processed;
	}
}

static void
deleteElem(ModifyGraphState *mgstate, Datum id, DelElemKind kind)
{
	EState	   *estate = mgstate->ps.state;
	Graphid		id_val;
	uint16		labid;
	Datum		values[SQLCMD_DEL_ELEM_NPARAMS];
	Oid			argTypes[SQLCMD_DEL_ELEM_NPARAMS] = {GRAPHIDOID};
	SqlcmdKey	key;
	SPIPlanPtr	plan;
	int			ret;

	id_val = DatumGetGraphid(id);
	labid = GraphidGetLabid(id_val);

	key.cmdtype = SQLCMD_TYPE_DEL_ELEM;
	key.labid = labid;
	plan = findPreparedPlan(&key);
	if (plan == NULL)
	{
		char		sqlcmd[SQLCMD_BUFLEN];
		char	   *relname;

		relname = get_rel_name(get_labid_relid(mgstate->graphid, labid));
		snprintf(sqlcmd, SQLCMD_BUFLEN, SQLCMD_DEL_ELEM,
				 mgstate->graphname, relname);

		plan = prepareSqlcmd(&key, sqlcmd, SQLCMD_DEL_ELEM_NPARAMS, argTypes);
	}

	values[0] = id;

	ret = SPI_execp(plan, values, NULL, 0);
	if (ret != SPI_OK_DELETE)
		elog(ERROR, "DEL_EDGES (%hu." INT64_FORMAT "): SPI_execp returned %d",
			 labid, GraphidGetLocid(id_val), ret);
	if (SPI_processed > 1)
		elog(ERROR, "DEL_EDGES (%hu." INT64_FORMAT "): only one or no element per execution must be deleted", labid, GraphidGetLocid(id_val));

	if (mgstate->canSetTag)
	{
		if (kind == DEL_ELEM_VERTEX)
		{
			Assert(estate->es_graphwrstats.deleteVertex != UINT_MAX);

			estate->es_graphwrstats.deleteVertex += SPI_processed;
		}
		else
		{
			Assert(estate->es_graphwrstats.deleteEdge != UINT_MAX);

			estate->es_graphwrstats.deleteEdge += SPI_processed;
		}
	}
}

static void
deletePath(ModifyGraphState *mgstate, Datum graphpath, bool detach)
{
	Datum		vertices_datum;
	Datum		edges_datum;
	AnyArrayType *vertices;
	AnyArrayType *edges;
	int			nvertices;
	int			nedges;
	ArrayAccessTypeInfo vertexInfo;
	ArrayAccessTypeInfo edgeInfo;
	array_iter	it;
	Datum		value;
	bool		null;
	int			i;

	getGraphpathArrays(graphpath, &vertices_datum, &edges_datum);

	vertices = DatumGetAnyArray(vertices_datum);
	edges = DatumGetAnyArray(edges_datum);

	nvertices = ArrayGetNItems(AARR_NDIM(vertices), AARR_DIMS(vertices));
	nedges = ArrayGetNItems(AARR_NDIM(edges), AARR_DIMS(edges));
	Assert(nvertices == nedges + 1);

	get_typlenbyvalalign(AARR_ELEMTYPE(vertices), &vertexInfo.typlen,
						 &vertexInfo.typbyval, &vertexInfo.typalign);
	get_typlenbyvalalign(AARR_ELEMTYPE(edges), &edgeInfo.typlen,
						 &edgeInfo.typbyval, &edgeInfo.typalign);

	/* delete edges first to avoid vertexHasEdge() */
	array_iter_setup(&it, edges);
	for (i = 0; i < nedges; i++)
	{
		value = array_iter_next(&it, &null, i, edgeInfo.typlen,
								edgeInfo.typbyval, edgeInfo.typalign);
		Assert(!null);

		deleteElem(mgstate, getEdgeIdDatum(value), DEL_ELEM_EDGE);
	}

	array_iter_setup(&it, vertices);
	for (i = 0; i < nvertices; i++)
	{
		value = array_iter_next(&it, &null, i, vertexInfo.typlen,
								vertexInfo.typbyval, vertexInfo.typalign);
		Assert(!null);

		deleteVertex(mgstate, value, detach);
	}
}

static TupleTableSlot *
ExecSetGraph(ModifyGraphState *mgstate, GSPKind kind, TupleTableSlot *slot)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	ExprContext *econtext = mgstate->ps.ps_ExprContext;
	ListCell   *ls;
	TupleTableSlot *result = mgstate->ps.ps_ResultTupleSlot;

	/*
	 * The results of previous clauses should be preserved.
	 * So, shallow copying is used.
	 */
	copyVirtualTupleTableSlot(result, slot);

	if (!mgstate->eagerness && SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	foreach(ls, mgstate->sets)
	{
		GraphSetProp *gsp = lfirst(ls);
		Oid			elemtype;
		Datum		elem_datum;
		Datum		id_datum;
		Datum		expr_datum;
		Datum		newelem;
		bool		isNull;
		ExprDoneCond isDone;
		MemoryContext oldmctx;

		if (gsp->kind != kind)
		{
			Assert(kind != GSP_NORMAL);
			continue;
		}

		elemtype = exprType((Node *) gsp->es_elem->expr);
		if (elemtype != VERTEXOID && elemtype != EDGEOID)
			elog(ERROR, "expected node or relationship");

		elem_datum = ExecEvalExpr(gsp->es_elem, econtext, &isNull, &isDone);
		if (isNull)
		{
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("updating NULL is not allowed")));
		}
		if (isDone != ExprSingleResult)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("expected single result")));
		}

		expr_datum = ExecEvalExpr(gsp->es_expr, econtext, &isNull, &isDone);
		if (isNull)
		{
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("property map cannot be NULL")));
		}
		if (isDone != ExprSingleResult)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("expected single result")));
		}

		if (elemtype == VERTEXOID)
			id_datum = getVertexIdDatum(elem_datum);
		else
			id_datum = getEdgeIdDatum(elem_datum);

		if (mgstate->eagerness)
		{
			enterSetPropTable(mgstate, id_datum, expr_datum);
		}
		else
		{
			updateElemProp(mgstate, id_datum, expr_datum);

			oldmctx = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

			newelem = makeModifiedElem(elem_datum, elemtype,
									   id_datum, expr_datum);

			MemoryContextSwitchTo(oldmctx);

			setSlotValueByName(result, newelem, gsp->variable);

		}
	}

	if (!mgstate->eagerness && SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return (plan->last ? NULL : result);
}

static void
updateElemProp(ModifyGraphState *mgstate, Datum id, Datum expr)
{
	EState	   *estate = mgstate->ps.state;
	Graphid		id_val;
	uint16		labid;
	Datum		values[SQLCMD_SET_PROP_NPARAMS];
	Oid			argTypes[SQLCMD_SET_PROP_NPARAMS] = {JSONBOID, GRAPHIDOID};
	SqlcmdKey	key;
	SPIPlanPtr	plan;
	int			ret;

	id_val = DatumGetGraphid(id);
	labid = GraphidGetLabid(id_val);

	key.cmdtype = SQLCMD_TYPE_SET_PROP;
	key.labid = labid;
	plan = findPreparedPlan(&key);
	if (plan == NULL)
	{
		char		sqlcmd[SQLCMD_BUFLEN];
		char	   *relname;

		relname = get_rel_name(get_labid_relid(mgstate->graphid, labid));
		snprintf(sqlcmd, SQLCMD_BUFLEN, SQLCMD_SET_PROP,
				 mgstate->graphname, relname);

		plan = prepareSqlcmd(&key, sqlcmd, SQLCMD_SET_PROP_NPARAMS, argTypes);
	}

	values[0] = expr;
	values[1] = id;

	ret = SPI_execp(plan, values, NULL, 0);
	if (ret != SPI_OK_UPDATE)
		elog(ERROR, "SET_PROP (%hu." INT64_FORMAT "): SPI_execp returned %d",
			 labid, GraphidGetLocid(id_val), ret);
	if (SPI_processed > 1)
		elog(ERROR, "SET_PROP (%hu." INT64_FORMAT "): only one element per execution must be updated", labid, GraphidGetLocid(id_val));

	if (mgstate->canSetTag)
	{
		Assert(estate->es_graphwrstats.updateProperty != UINT_MAX);

		estate->es_graphwrstats.updateProperty += SPI_processed;
	}
}

static Datum
makeModifiedElem(Datum elem, Oid elemtype, Datum id, Datum prop_map)
{
	if (elemtype == VERTEXOID)
	{
		return makeGraphVertexDatum(id, prop_map);
	}
	else
	{
		Datum		start;
		Datum		end;

		start = getEdgeStartDatum(elem);
		end = getEdgeEndDatum(elem);

		return makeGraphEdgeDatum(id, start, end, prop_map);
	}
}

static TupleTableSlot *
ExecMergeGraph(ModifyGraphState *mgstate, TupleTableSlot *slot)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	ExprContext *econtext = mgstate->ps.ps_ExprContext;
	GraphPath *path = (GraphPath *) linitial(mgstate->pattern);

	ResetExprContext(econtext);
	econtext->ecxt_scantuple = slot;

	if (isMatchedMergePattern(mgstate->subplan))
	{
		if (mgstate->sets != NIL)
			slot = ExecSetGraph(mgstate, GSP_ON_MATCH, slot);
	}
	else
	{
		slot = createMergePath(mgstate, path, slot);

		if (mgstate->sets != NIL)
		{
			/*
			 * Increase CommandId to scan tuples created by createMergePath().
			 */
			while (mgstate->ps.state->es_output_cid >= GetCurrentCommandId(true))
				CommandCounterIncrement();

			slot = ExecSetGraph(mgstate, GSP_ON_CREATE, slot);
		}
	}

	return (plan->last ? NULL : slot);
}

/* tricky but efficient */
static bool
isMatchedMergePattern(PlanState *planstate)
{
	Assert(IsA(planstate, NestLoopState));

	return ((NestLoopState *) planstate)->nl_MatchedOuter;
}

static TupleTableSlot *
createMergePath(ModifyGraphState *mgstate, GraphPath *path,
				TupleTableSlot *slot)
{
	ExprContext *econtext = mgstate->ps.ps_ExprContext;
	bool		out = (path->variable != NULL);
	int			pathlen;
	Datum	   *vertices = NULL;
	Datum	   *edges = NULL;
	int			nvertices;
	int			nedges;
	ListCell   *le;
	Graphid		prevvid = 0;
	GraphEdge  *gedge = NULL;

	if (out)
	{
		pathlen = list_length(path->chain);
		Assert(pathlen % 2 == 1);

		vertices = makeDatumArray(econtext, (pathlen / 2) + 1);
		edges = makeDatumArray(econtext, pathlen / 2);

		nvertices = 0;
		nedges = 0;
	}

	foreach(le, path->chain)
	{
		Node *elem = (Node *) lfirst(le);

		if (IsA(elem, GraphVertex))
		{
			GraphVertex *gvertex = (GraphVertex *) elem;
			Graphid		vid;
			Datum		vertex;

			vertex = findVertex(slot, gvertex, &vid);
			if (vertex == DATUM_NULL)
				vertex = createMergeVertex(mgstate, gvertex, &vid, slot);

			if (out)
				vertices[nvertices++] = vertex;

			if (gedge != NULL)
			{
				Datum edge;

				edge = findEdge(slot, gedge, NULL);
				Assert(edge == DATUM_NULL);

				if (gedge->direction == GRAPH_EDGE_DIR_LEFT)
				{
					edge = createMergeEdge(mgstate, gedge, vid, prevvid, slot);
				}
				else
				{
					Assert(gedge->direction == GRAPH_EDGE_DIR_RIGHT);

					edge = createMergeEdge(mgstate, gedge, prevvid, vid, slot);
				}

				if (out)
					edges[nedges++] = edge;
			}

			prevvid = vid;
		}
		else
		{
			Assert(IsA(elem, GraphEdge));

			gedge = (GraphEdge *) elem;
		}
	}

	/* make a graphpath and set it to the slot */
	if (out)
	{
		MemoryContext oldmctx;
		Datum graphpath;

		Assert(nvertices == nedges + 1);
		Assert(pathlen == nvertices + nedges);

		oldmctx = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		graphpath = makeGraphpathDatum(vertices, nvertices, edges, nedges);

		MemoryContextSwitchTo(oldmctx);

		setSlotValueByName(slot, graphpath, path->variable);
	}

	return slot;
}

static Datum
createMergeVertex(ModifyGraphState *mgstate, GraphVertex *gvertex,
				  Graphid *vid, TupleTableSlot *slot)
{
	EState		   *estate = mgstate->ps.state;
	ExprContext	   *econtext = mgstate->ps.ps_ExprContext;
	ResultRelInfo  *resultRelInfo;
	ResultRelInfo  *savedResultRelInfo;
	bool			isNull;
	ExprDoneCond 	isDone;
	Datum			vertex;
	Datum			vertexId;
	Datum			vertexProp;
	TupleTableSlot *insertSlot = mgstate->elemTupleSlot;
	HeapTuple		tuple;

	resultRelInfo = getResultRelInfo(mgstate, gvertex->relid);
	savedResultRelInfo = estate->es_result_relation_info;
	estate->es_result_relation_info = resultRelInfo;

	vertex = ExecEvalExpr(gvertex->es_expr, econtext, &isNull, &isDone);
	if (isNull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("NULL is not allowed in MERGE")));
	if (isDone != ExprSingleResult)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("expected single result")));

	if (gvertex->es_qual != NULL &&
		ExecQual((List *) gvertex->es_qual, econtext, false))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use null property value in MERGE")));

	vertexId = getVertexIdDatum(vertex);
	*vid = DatumGetGraphid(vertexId);

	vertexProp = getVertexPropDatum(vertex);
	if (!JB_ROOT_IS_OBJECT(DatumGetJsonb(vertexProp)))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("jsonb object is expected for property map")));

	ExecClearTuple(insertSlot);

	ExecSetSlotDescriptor(insertSlot,
						  RelationGetDescr(resultRelInfo->ri_RelationDesc));
	insertSlot->tts_values[0] = vertexId;
	insertSlot->tts_values[1] = vertexProp;
	MemSet(insertSlot->tts_isnull, false,
		   insertSlot->tts_tupleDescriptor->natts * sizeof(bool));
	ExecStoreVirtualTuple(insertSlot);

	tuple = ExecMaterializeSlot(insertSlot);

	/*
	 * Constraints might reference the tableoid column, so initialize
	 * t_tableOid before evaluating them.
	 */
	tuple->t_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	/*
	 * Check the constraints of the tuple
	 */
	if (resultRelInfo->ri_RelationDesc->rd_att->constr != NULL)
		ExecConstraints(resultRelInfo, insertSlot, estate);

	/*
	 * insert the tuple normally
	 */
	heap_insert(resultRelInfo->ri_RelationDesc, tuple, estate->es_output_cid,
				0, NULL);

	/* insert index entries for the tuple */
	if (resultRelInfo->ri_NumIndices > 0)
		ExecInsertIndexTuples(insertSlot, &(tuple->t_self), estate, false,
							  NULL, NIL);

	if (gvertex->resno > 0)
		setSlotValueByAttnum(slot, vertex, gvertex->resno);

	if (mgstate->canSetTag)
	{
		Assert(estate->es_graphwrstats.insertVertex != UINT_MAX);

		estate->es_graphwrstats.insertVertex++;
	}

	estate->es_result_relation_info = savedResultRelInfo;

	return vertex;
}

static Datum
createMergeEdge(ModifyGraphState *mgstate, GraphEdge *gedge, Graphid start,
				Graphid end, TupleTableSlot *slot)
{
	EState		   *estate = mgstate->ps.state;
	ExprContext	   *econtext = mgstate->ps.ps_ExprContext;
	ResultRelInfo  *resultRelInfo;
	ResultRelInfo  *savedResultRelInfo;
	bool			isNull;
	ExprDoneCond	isDone;
	Datum			edge;
	Datum			edgeProp;
	TupleTableSlot *insertSlot = mgstate->elemTupleSlot;
	HeapTuple		tuple;

	resultRelInfo = getResultRelInfo(mgstate, gedge->relid);
	savedResultRelInfo = estate->es_result_relation_info;
	estate->es_result_relation_info = resultRelInfo;

	edge = ExecEvalExpr(gedge->es_expr, econtext, &isNull, &isDone);
	if (isNull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("NULL is not allowed in MERGE")));
	if (isDone != ExprSingleResult)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("expected single result")));

	if (gedge->es_qual != NULL &&
		ExecQual((List *) gedge->es_qual, econtext, false))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use null property value in MERGE")));

	edgeProp = getEdgePropDatum(edge);
	if (!JB_ROOT_IS_OBJECT(DatumGetJsonb(edgeProp)))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("jsonb object is expected for property map")));

	ExecClearTuple(insertSlot);

	ExecSetSlotDescriptor(insertSlot,
						  RelationGetDescr(resultRelInfo->ri_RelationDesc));
	insertSlot->tts_values[0] = getEdgeIdDatum(edge);
	insertSlot->tts_values[1] = GraphidGetDatum(start);
	insertSlot->tts_values[2] = GraphidGetDatum(end);
	insertSlot->tts_values[3] = edgeProp;
	MemSet(insertSlot->tts_isnull, false,
		   insertSlot->tts_tupleDescriptor->natts * sizeof(bool));
	ExecStoreVirtualTuple(insertSlot);

	tuple = ExecMaterializeSlot(insertSlot);

	tuple->t_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	if (resultRelInfo->ri_RelationDesc->rd_att->constr != NULL)
		ExecConstraints(resultRelInfo, insertSlot, estate);

	heap_insert(resultRelInfo->ri_RelationDesc, tuple, estate->es_output_cid,
				0, NULL);

	if (resultRelInfo->ri_NumIndices > 0)
		ExecInsertIndexTuples(insertSlot, &(tuple->t_self), estate, false,
							  NULL, NIL);

	edge = makeGraphEdgeDatum(insertSlot->tts_values[0],
							  insertSlot->tts_values[1],
							  insertSlot->tts_values[2],
							  insertSlot->tts_values[3]);

	if (gedge->resno > 0)
		setSlotValueByAttnum(slot, edge, gedge->resno);

	if (mgstate->canSetTag)
	{
		Assert(estate->es_graphwrstats.insertEdge != UINT_MAX);

		estate->es_graphwrstats.insertEdge++;
	}


	estate->es_result_relation_info = savedResultRelInfo;

	return edge;
}

static TupleTableSlot *
copyVirtualTupleTableSlot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	int natts = srcslot->tts_tupleDescriptor->natts;

	ExecSetSlotDescriptor(dstslot, srcslot->tts_tupleDescriptor);

	/* shallow copy */
	memcpy(dstslot->tts_values, srcslot->tts_values, natts * sizeof(Datum));
	memcpy(dstslot->tts_isnull, srcslot->tts_isnull, natts * sizeof(bool));

	ExecStoreVirtualTuple(dstslot);

	return dstslot;
}

/* 
 * NOTE: What happens if there is a multiple execution of ModifyGraph?
 */
static void
InitSqlcmdHashTable(MemoryContext mcxt)
{
	HASHCTL ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(SqlcmdKey);
	ctl.entrysize = sizeof(SqlcmdEntry);
	ctl.hcxt = mcxt;

	sqlcmd_cache = hash_create("ModifyGraph SPIPlan cache", 128, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * NOTE: If an error occurs during the execution of ModifyGraph,
 *       there is no way to remove saved plans.
 */
static void
EndSqlcmdHashTable(void)
{
	HASH_SEQ_STATUS seqStatus;
	SqlcmdEntry *entry;

	hash_seq_init(&seqStatus, sqlcmd_cache);
	while ((entry = hash_seq_search(&seqStatus)) != NULL)
		SPI_freeplan(entry->plan);

	hash_destroy(sqlcmd_cache);

	sqlcmd_cache = NULL;
}

static SPIPlanPtr
findPreparedPlan(SqlcmdKey *key)
{
	SqlcmdEntry *entry;
	SPIPlanPtr plan;

	Assert(sqlcmd_cache != NULL);

	entry = hash_search(sqlcmd_cache, (void *) key, HASH_FIND, NULL);
	if (entry == NULL)
		return NULL;

	plan = entry->plan;
	if (plan && SPI_plan_is_valid(plan))
		return plan;

	entry->plan = NULL;
	if (plan != NULL)
		SPI_freeplan(plan);

	return NULL;
}

static SPIPlanPtr
prepareSqlcmd(SqlcmdKey *key, char *sqlcmd, int nargs, Oid *argtypes)
{
	SPIPlanPtr plan;

	plan = SPI_prepare(sqlcmd, nargs, argtypes);
	if (plan == NULL)
		elog(ERROR, "failed to SPI_prepare(): %d", SPI_result);

	savePreparedPlan(key, plan);

	return plan;
}

static void
savePreparedPlan(SqlcmdKey *key, SPIPlanPtr plan)
{
	SqlcmdEntry *entry;
	bool		found;

	Assert(sqlcmd_cache != NULL);

	if (SPI_keepplan(plan))
		elog(ERROR, "savePreparedPlan: SPI_keepplan failed");

	entry = hash_search(sqlcmd_cache, (void *) key, HASH_ENTER, &found);
	Assert(!found || entry->plan == NULL);
	entry->plan = plan;
}

static void
enterSetPropTable(ModifyGraphState *mgstate, Datum gid, Datum prop)
{
	bool		found;
	ModifiedPropEntry *entry;

	entry = hash_search(mgstate->propTable, (void *) &gid, HASH_ENTER, &found);
	if (found)
		pfree((void *) entry->val.properties);
	entry->val.properties = datumCopy(prop, false, -1);
}

static void
enterDelPropTable(ModifyGraphState *mgstate, Datum elem, Oid type)
{
	Datum gid;
	ModifiedPropEntry *entry;

	if (type == VERTEXOID)
	{
		gid = getVertexIdDatum(elem);

		entry = hash_search(mgstate->propTable, (void *) &gid, HASH_ENTER,
							NULL);
		entry->val.kind = DEL_ELEM_VERTEX;
	}
	else if (type == EDGEOID)
	{
		gid = getEdgeIdDatum(elem);

		entry = hash_search(mgstate->propTable, (void *) &gid, HASH_ENTER,
							NULL);
		entry->val.kind = DEL_ELEM_EDGE;
	}
	else
	{
		List	   *vtxGidList = NIL;
		List	   *edgeGidList = NIL;
		ListCell   *lc;

		Assert(type == GRAPHPATHOID);

		getGidListInPath(elem, &vtxGidList, &edgeGidList);

		foreach(lc, vtxGidList)
		{
			gid = (Datum) lfirst(lc);

			entry = hash_search(mgstate->propTable,
								(void *) &gid, HASH_ENTER, NULL);
			entry->val.kind = DEL_ELEM_VERTEX;
		}

		foreach(lc, edgeGidList)
		{
			gid = (Datum) lfirst(lc);

			entry = hash_search(mgstate->propTable,
								(void *) &gid, HASH_ENTER, NULL);
			entry->val.kind = DEL_ELEM_EDGE;
		}
	}
}

static void
getGidListInPath(Datum graphpath, List **vtxlist, List **edgelist)
{
	Datum		vertices_datum;
	Datum		edges_datum;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	array_iter	it;
	Datum		value;
	bool		isnull;
	int			i;

	getGraphpathArrays(graphpath, &vertices_datum, &edges_datum);

	if (vtxlist != NULL)
	{
		AnyArrayType *vertices;
		int			nvertices;

		vertices = DatumGetAnyArray(vertices_datum);
		nvertices = ArrayGetNItems(AARR_NDIM(vertices), AARR_DIMS(vertices));

		get_typlenbyvalalign(AARR_ELEMTYPE(vertices), &typlen,
							 &typbyval, &typalign);
		array_iter_setup(&it, vertices);
		for (i = 0; i < nvertices; i++)
		{
			value = array_iter_next(&it, &isnull, i, typlen,
									typbyval, typalign);
			Assert(!isnull);

			*vtxlist = lappend(*vtxlist,
							   DatumGetPointer(getVertexIdDatum(value)));
		}
	}

	if (edgelist != NULL)
	{
		AnyArrayType *edges;
		int			nedges;

		edges = DatumGetAnyArray(edges_datum);
		nedges = ArrayGetNItems(AARR_NDIM(edges), AARR_DIMS(edges));

		get_typlenbyvalalign(AARR_ELEMTYPE(edges), &typlen,
							 &typbyval, &typalign);
		array_iter_setup(&it, edges);
		for (i = 0; i < nedges; i++)
		{
			value = array_iter_next(&it, &isnull, i,typlen,
									typbyval, typalign);
			Assert(!isnull);

			*edgelist = lappend(*edgelist,
								DatumGetPointer(getEdgeIdDatum(value)));
		}
	}
}

static Datum
getVertexFinalPropMap(ModifyGraphState *mgstate, Datum origin, Graphid gid)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	ModifiedPropEntry *entry;

	entry = hash_search(mgstate->propTable, (void *) &gid, HASH_FIND, NULL);

	/* un-modified vertex */
	if (entry == NULL)
		return origin;

	if (plan->operation == GWROP_DELETE)
		return (Datum) NULL;
	else
		return makeGraphVertexDatum(gid, entry->val.properties);
}

static Datum
getEdgeFinalPropMap(ModifyGraphState *mgstate, Datum origin, Graphid gid)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	ModifiedPropEntry *entry;

	entry = hash_search(mgstate->propTable, (void *) &gid, HASH_FIND, NULL);

	/* un-modified edge */
	if (entry == NULL)
		return origin;

	if (plan->operation == GWROP_DELETE)
	{
		return (Datum) NULL;
	}
	else
	{
		Datum		start;
		Datum		end;

		start = getEdgeStartDatum(origin);
		end = getEdgeEndDatum(origin);

		return makeGraphEdgeDatum(gid, start, end, entry->val.properties);
	}
}

static Datum
getPathFinalPropMap(ModifyGraphState *mgstate, Datum origin)
{
	Datum		vertices_datum;
	Datum		edges_datum;
	AnyArrayType *arr_vertices;
	AnyArrayType *arr_edges;
	int			nvertices;
	int			nedges;
	Datum	   *vertices;
	Datum	   *edges;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	array_iter	it;
	int			i;
	Datum		value;
	Graphid		gid;
	bool		isnull;
	bool		modified = false;
	Datum		result;

	getGraphpathArrays(origin, &vertices_datum, &edges_datum);

	arr_vertices = DatumGetAnyArray(vertices_datum);
	arr_edges = DatumGetAnyArray(edges_datum);

	nvertices = ArrayGetNItems(AARR_NDIM(arr_vertices),
							   AARR_DIMS(arr_vertices));
	nedges = ArrayGetNItems(AARR_NDIM(arr_edges), AARR_DIMS(arr_edges));
	Assert(nvertices == nedges + 1);

	vertices = palloc(nvertices * sizeof(Datum));
	edges = palloc(nedges * sizeof(Datum));

	get_typlenbyvalalign(AARR_ELEMTYPE(arr_vertices), &typlen,
						 &typbyval, &typalign);
	array_iter_setup(&it, arr_vertices);
	for (i = 0; i < nvertices; i++)
	{
		Datum		vertex;

		value = array_iter_next(&it, &isnull, i, typlen, typbyval, typalign);
		Assert(!isnull);

		gid = getVertexIdDatum(value);
		vertex = getVertexFinalPropMap(mgstate, value, gid);

		if (vertex == (Datum) NULL)
			elog(ERROR, "cannot delete a vertex in a graphpath");

		if (vertex != value)
			modified = true;

		vertices[i] = vertex;
	}

	get_typlenbyvalalign(AARR_ELEMTYPE(arr_edges), &typlen,
						 &typbyval, &typalign);
	array_iter_setup(&it, arr_edges);
	for (i = 0; i < nedges; i++)
	{
		Datum		edge;

		value = array_iter_next(&it, &isnull, i, typlen, typbyval, typalign);
		Assert(!isnull);

		gid = getEdgeIdDatum(value);
		edge = getEdgeFinalPropMap(mgstate, value, gid);

		if (edge == (Datum) NULL)
			elog(ERROR, "cannot modify the element of graphpath.");

		if (edge != value)
			modified = true;

		edges[i] = edge;
	}

	if (modified)
		result = makeGraphpathDatum(vertices, nvertices, edges, nedges);
	else
		result = origin;

	pfree(vertices);
	pfree(edges);

	return result;
}

static void
reflectModifiedProp(ModifyGraphState *mgstate)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	HASH_SEQ_STATUS seq;
	ModifiedPropEntry *entry;

	Assert(mgstate->propTable != NULL);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	DisableGraphDML = false;

	hash_seq_init(&seq, mgstate->propTable);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		Datum gid = GraphidGetDatum(entry->key);

		/* write the object to heap */
		if (plan->operation == GWROP_DELETE)
			deleteElem(mgstate, gid, entry->val.kind);
		else
			updateElemProp(mgstate, gid, entry->val.properties);
	}

	DisableGraphDML = true;

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
}
