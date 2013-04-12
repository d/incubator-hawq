/*-------------------------------------------------------------------------
 *
 * cdbquerydispatchingcontext.c
 *
 * Copyright (c) 2007-2013, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/fcntl.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/aoseg.h"
#include "catalog/catalog.h"
#include "catalog/gp_fastsequence.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_exttable.h"
#include "catalog/pg_magic_oid.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_tablespace.h"
#include "catalog/gp_fastsequence.h"
#include "cdb/cdbdispatchedtablespaceinfo.h"
#include "cdb/cdbpartition.h"
#include "cdb/cdbquerycontextdispatching.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "commands/dbcommands.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "optimizer/prep.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/* include last until fix dependencies in this file */
#include "access/aocssegfiles.h"


/**
 * all opened file on shared storage will be removed after transaction terminated.
 */
struct DorpListItemData
{
    char *path;
    struct DorpListItemData *next;
};
typedef struct DorpListItemData DorpListItemData;

typedef struct DorpListItemData *DorpListItem;

static DorpListItem DropList = NULL;

int QueryContextDispatchingSizeMemoryLimit = 100 * 1024; /* KB */

/*
 * |------type id : 1 byte---------|
 * |---content length : 4 byte-----|
 * |---variable length content-----|
 */

/*
 * |------type id : 1 byte---------|
 * |-----fixed length content -----|
 */

enum QueryContextDispatchingItemType
{
    MasterXid, TablespaceLocation, TupleType, EmptyTable
};
typedef enum QueryContextDispatchingItemType QueryContextDispatchingItemType;

enum QueryContextDispatchingObjType
{
    RelationType, TablespaceType, NamespaceType
};
typedef enum QueryContextDispatchingObjType QueryContextDispatchingObjType;

struct QueryContextDispatchingHashItem
{
    Oid objid;
    QueryContextDispatchingObjType type;
};
typedef struct QueryContextDispatchingHashItem QueryContextDispatchingHashItem;

static void
prepareDispatchedCatalogFunctionExpr(QueryContextInfo *cxt, Expr *expr, HTAB *htab);

static void
prepareDispatchedCatalogForAoCo(QueryContextInfo *cxt, Oid relid,
        bool forInsert, int32 segno, HTAB *htab);

static void
prepareDispatchedCatalogExternalTable(QueryContextInfo *cxt, Oid relid);

/**
 * construct the file location for query context dispatching.
 */
static void
GetQueryContextDipsatchingFileLocation(char **buffer)
{
    int len, content = 0;

    static int count = 0;

    char *path = NULL, *pos;
    Oid tablespace = get_database_dts(MyDatabaseId);

    Assert(GpIdentity.numsegments >= 1);
    GetFilespacePathForTablespace(tablespace, content, &path);

    Assert(NULL != path);

    pos = path + strlen(path) - 1;
    Assert('0' == *pos);

    *pos = '\0';

    len = strlen(path);
    len += 64;

    *buffer = palloc(len);

    snprintf(*buffer, len, "%s-1/session_%d_id_%d_query_context", path, gp_session_id, ++count);

    pfree(path);

}
/*
 * create a QueryContextInfo and fill the location of file on shared storage.
 *
 * also add auxiliary information such as master's transaction into QueryContextInfo.
 */
QueryContextInfo *
CreateQueryContextInfo(void)
{
    QueryContextInfo *retval = makeNode(QueryContextInfo);

    GetQueryContextDipsatchingFileLocation(&retval->sharedPath);

    AddAuxInfoToQueryContextInfo(retval);

    return retval;

}

/*
 * On QE, construct a QueryContextInfo from a file on shared storage.
 */
void
InitQueryContextInfoFromFile(QueryContextInfo *cxt)
{
    Assert(NULL != cxt->sharedPath);

    cxt->file = PathNameOpenFile(cxt->sharedPath, O_RDONLY, 0);

}

/*
 * clsoe  a QueryContextInfo and close its file if it is opened.
 */
void
CloseQueryContextInfo(QueryContextInfo *cxt)
{
    Assert(cxt);

    if (Debug_querycontext_print && Gp_role != GP_ROLE_EXECUTE)
    {
        int size = cxt->cursor;
        if (cxt->useFile)
            size = FileNonVirtualTell(cxt->file);

        elog(LOG, "query context size: %d bytes, passed by %s",
                size, (cxt->useFile? "shared storage" : "dispatching"));

    }

    if (cxt->useFile)
    {
        FileClose(cxt->file);
        cxt->file = 0;
    }
}

/*
 * cleanup and destory a QueryContextInfo
 */
void
DropQueryContextInfo(QueryContextInfo *cxt)
{
    Assert(NULL != cxt);

    if (TestFileValid(cxt->file))
        FileClose(cxt->file);

    if (cxt->sharedPath)
        pfree(cxt->sharedPath);

    if (cxt->buffer)
        pfree(cxt->buffer);

    pfree(cxt);
}

/*
 * add a file to drop list.
 */
static void
AddToDropList(const char *path)
{
    MemoryContext old;
    old = MemoryContextSwitchTo(TopTransactionContext);

    DorpListItem p = palloc(sizeof(DorpListItemData));
    p->path = palloc(strlen(path) + 1);
    strcpy(p->path, path);

    p->next = DropList;
    DropList = p;

    MemoryContextSwitchTo(old);
}

/*
 * called when transaction commit/rollback to drop all pending files.
 */
void
AtEOXact_QueryContext(void)
{
    DorpListItem p, q;
    for (p = DropList; NULL != p;)
    {
        Assert(p->path);
        RemovePath(p->path, FALSE);

        q = p;
        p = p->next;

        pfree(q->path);
        pfree(q);
    }

    DropList = NULL;
}

/*
 * read from file until get expected size of bytes or reach end of file.
 * return TRUE if read successfully, return FALSE when reach end of file.
 *
 * report error if reach end of file and errorOnEof is true.
 */
static bool
ReadFully(File file, const char *path, char *buffer, int size,
        bool errorOnEof)
{
    int done, todo = size;

    while (todo > 0)
    {
        done = FileRead(file, buffer + size - todo, todo);

        if (0 == done && errorOnEof)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_IO_ERROR), errmsg( "rebuild query context failed:"
                    " can not read data since unexpected eof")));
        }
        else if (0 == done)
        {
            return FALSE;
        }
        else if (done < 0)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_IO_ERROR), errmsg( "rebuild query context failed:"
                    " can not read data from %s", path)));
        }

        todo -= done;
    }

    return TRUE;
}

/*
 * read from QueryContextInfo until get expected size of bytes or reach end of file.
 * return TRUE if read successfully, return FALSE when reach end of file.
 *
 * report error if reach end of file and errorOnEof is true.
 */
static bool
ReadData(QueryContextInfo *cxt, char *buffer, int size,
        bool errorOnEof)
{
    Assert(NULL != cxt);
    Assert(NULL != buffer);
    Assert(size > 0);

    if (!cxt->useFile)
    {
        Assert(cxt->buffer);
        if (cxt->size == cxt->cursor && errorOnEof)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_IO_ERROR), errmsg( "rebuild query context failed: "
                    "can not read data since unexpected eof")));
        }
        else if (cxt->size == cxt->cursor)
        {
            return FALSE;
        }
        else if (cxt->size - cxt->cursor < size)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_IO_ERROR), errmsg( "rebuild query context failed: "
                    "can not read data since unexpected eof")));

        }

        memcpy(buffer, cxt->buffer + cxt->cursor, size);
        cxt->cursor += size;

        return TRUE;

    }
    else
    {
        Assert(TestFileValid(cxt->file));
        return ReadFully(cxt->file, cxt->sharedPath, buffer, size, errorOnEof);
    }
}

/*
 * write data into QueryContextInfo
 */
static void
WriteData(QueryContextInfo *cxt, const char *buffer, int size)
{

    Assert(NULL != cxt);
    Assert(NULL != buffer);
    Assert(size > 0);

    if (cxt->useFile)
    {
        Assert(TestFileValid(cxt->file));

        if (size != FileWrite(cxt->file, buffer, size))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_IO_ERROR), errmsg( "prepare query context failed: "
                    "can not write data into file: %s", cxt->sharedPath)));
        }

        return;
    }

    /* switch to file*/
    if (cxt->cursor + size > QueryContextDispatchingSizeMemoryLimit *1024)
    {
        elog(LOG, "Switch to use file on shared storage : %s "
                "instead of dispatching the query context "
                    "since memory usage exceed the gp_query_context_mem_limit", cxt->sharedPath);

        cxt->useFile = TRUE;

        char *p;
        char *dir = palloc(strlen(cxt->sharedPath) + 1);
        strcpy(dir, cxt->sharedPath);
        p = strrchr(dir, '/');

        if (NULL == p)
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR), errmsg("failed to construct"
                    " query context directory, file path: %s", cxt->sharedPath)));

        }
        *p = 0;

        MakeDirectory(dir, 0400);

        cxt->file = PathNameOpenFile(cxt->sharedPath, O_CREAT | O_WRONLY, 0400);

        AddToDropList(cxt->sharedPath);

        if (cxt->cursor > 0)
        {
            if (cxt->cursor != FileWrite(cxt->file, cxt->buffer, cxt->cursor))
            {
                ereport(ERROR,
                        (errcode(ERRCODE_IO_ERROR), errmsg( "prepare query context failed: "
                        "can not write data into file: %s", cxt->sharedPath)));
            }
        }
        if (cxt->buffer)
            pfree(cxt->buffer);
        cxt->buffer = NULL;
        cxt->size = cxt->cursor = 0;

        WriteData(cxt, buffer, size);
    }
    else
    {
        if (cxt->cursor + size > cxt->size)
        {
            cxt->size = (1 + cxt->size) * 2;

            if (cxt->size < cxt->cursor + size)
                cxt->size = cxt->cursor + size;

            if (cxt->size > QueryContextDispatchingSizeMemoryLimit * 1024)
                cxt->size = QueryContextDispatchingSizeMemoryLimit * 1024;

            if (cxt->buffer)
                cxt->buffer = repalloc(cxt->buffer, cxt->size);
            else
                cxt->buffer = palloc(cxt->size);
        }
        memcpy(cxt->buffer + cxt->cursor, buffer, size);
        cxt->cursor += size;
    }
}

/*
 * get and set master's transaction on QE
 */
static void
RebuildMasterTransactionId(QueryContextInfo *cxt)
{
    Assert(Gp_role == GP_ROLE_EXECUTE);

    TransactionId masterXid;

    char buffer[4];
    ReadData(cxt, buffer, sizeof(buffer), TRUE);

    masterXid = (TransactionId) ntohl(*(uint32 *)buffer);

    SetMasterTransactionId(masterXid);

}

/*
 * build a new empty in-memory table with given relid.
 */
static void
RebuildEmptyTable(QueryContextInfo *cxt)
{
    Oid relid;
    char buffer[4];

    Relation rel;
    InMemHeapRelation inmemrel;

    ReadData(cxt, buffer, sizeof(buffer), TRUE);

    relid = (Oid) ntohl(*(uint32 *)buffer);

    inmemrel = OidGetInMemHeapRelation(relid);
    Assert(NULL == inmemrel);

    rel = heap_open(relid, AccessShareLock);
    inmemrel = InMemHeap_Create(relid, rel, TRUE, 10, AccessShareLock,
            RelationGetRelationName(rel));
    Assert(NULL != inmemrel);

}

/*
 * get and set a tablespace location on QE.
 */
static void
RebuildTablespaceLocation(QueryContextInfo *cxt)
{
    Assert(Gp_role == GP_ROLE_EXECUTE);

    int32 len;
    Oid tspoid;
    char location[FilespaceLocationBlankPaddedWithNullTermLen];

    char *buffer, *loc;
    buffer = palloc(sizeof(int32));

    ReadData(cxt, buffer, sizeof(int32), TRUE);

    len = (TransactionId) ntohl(*(uint32 *)buffer);
    buffer = repalloc(buffer, len);

    ReadData(cxt, buffer, len, TRUE);
    tspoid = (Oid) ntohl(*(uint32 *)buffer);

    loc = buffer + sizeof(Oid);

    snprintf(location, FilespaceLocationBlankPaddedWithNullTermLen,
            buffer + sizeof(Oid), GpIdentity.segindex);

    DispatchedFilespace_AddForTablespace(tspoid, location);

    pfree(buffer);

}

/*
 * get and insert a tuple into in-memory table.
 */
static void
RebuildTupleForRelation(QueryContextInfo *cxt)
{
    Assert(Gp_role == GP_ROLE_EXECUTE);

    Oid relid;
    int32 len, contentid, tuplen;

    char *buffer;
    Relation rel;
    InMemHeapRelation inmemrel;

    HeapTuple tuple;

    buffer = palloc(sizeof(int32));

    ReadData(cxt, buffer, sizeof(int32), TRUE);
    len = (int32) ntohl(*(uint32*)buffer);

    ReadData(cxt, buffer, sizeof(int32), TRUE);
    relid = (Oid) ntohl(*(uint32*)buffer);

    ReadData(cxt, buffer, sizeof(int32), TRUE);
    contentid = (int32) ntohl(*(uint32*)buffer);

    tuplen = len - sizeof(Oid) - sizeof(int32);
    Assert(tuplen > 0);

    buffer = repalloc(buffer, tuplen);
    ReadData(cxt, buffer, tuplen, TRUE);

    if (contentid == MASTER_CONTENT_ID || contentid == GpIdentity.segindex)
    {
        tuple = (HeapTuple) buffer;
        tuple->t_data = (HeapTupleHeader) ((char *) tuple
                + sizeof(HeapTupleData));

        inmemrel = OidGetInMemHeapRelation(relid);

        if (NULL == inmemrel)
        {
            rel = heap_open(relid, AccessShareLock);
            inmemrel = InMemHeap_Create(relid, rel, TRUE, 10, AccessShareLock,
                    RelationGetRelationName(rel));
            Assert(NULL != inmemrel);
        }

        InMemHeap_Insert(inmemrel, tuple, contentid);

        if (Debug_querycontext_print)
        {
            elog( LOG, "Query Context: rebuild tuple for relation %s, relid %u:",
                    inmemrel->relname, inmemrel->relid);
        }

        if (Debug_querycontext_print_tuple)
        {
            elog( LOG, "rebuild query context tuple for relation:"
                    " %s, total bytes: %d",inmemrel->relname,
                    (int)(len + sizeof(char) + sizeof(int32)));
        }

        pfree(buffer);
    }
}

/*
 * rebuild execute context
 */
void
RebuildQueryContext(QueryContextInfo *cxt)
{
    char type;

    while (TRUE)
    {
        if (!ReadData(cxt, &type, sizeof(char), FALSE))
            break;

        switch (type)
        {
        case MasterXid:
            RebuildMasterTransactionId(cxt);
            break;
        case TablespaceLocation:
            RebuildTablespaceLocation(cxt);
            break;
        case TupleType:
            RebuildTupleForRelation(cxt);
            break;
        case EmptyTable:
            RebuildEmptyTable(cxt);
            break;
        default:
            ereport(ERROR,
                    (errcode(ERRCODE_GP_INTERNAL_ERROR), errmsg( "unrecognized "
                    "QueryContextDispatchingItemType %d", (int) type)));
        }
    }

	/*
	 * This is a bit overkill, but since we don't yet have a decent way to
	 * determine individual cache entries affected by the dispatched
	 * metadata, we should release all the system cache here.  Our good-old
	 * shared invalidation message is not necessarily applicable here;
	 * backends in segments never change the metadata and we care the
	 * metadata changes only within the session.
	 */
	ResetSystemCaches();
}

/*
 * collect QD's transaction id and other global variables
 */
void
AddAuxInfoToQueryContextInfo(QueryContextInfo *cxt)
{
    Assert(NULL != cxt);

    StringInfoData buffer;
    initStringInfo(&buffer);

    /*
     * add master's transaction id. It will be used in external table
     */
    pq_sendint(&buffer, (int) MasterXid, sizeof(char));
    pq_sendint(&buffer, GetMasterTransactionId(), sizeof(TransactionId));

    /*
     * may add other here
     */
    WriteData(cxt, buffer.data, buffer.len);

    pfree(buffer.data);
}

/*
 * in some special case such as pg_aoseg_xxx,
 * it is a heap relation but act as a catalog.
 * and it can be empty.
 *
 * if it is empty, send this flag to QE to build a empty in-memory table.
 */
void
AddEmptyTableFlagToContextInfo(QueryContextInfo *cxt, Oid relid)
{
    StringInfoData buffer;
    initStringInfo(&buffer);

    pq_sendint(&buffer, EmptyTable, sizeof(char));
    pq_sendint(&buffer, relid, sizeof(Oid));

    WriteData(cxt, buffer.data, buffer.len);

    pfree(buffer.data);
}

/*
 * collect tuples for one catalog table
 */
void
AddTupleToContextInfo(QueryContextInfo *cxt, Oid relid,
        const char *relname, HeapTuple tuple, int32 contentid)
{
    Assert(NULL != cxt);

    StringInfoData header;
    initStringInfo(&header);

    int32 tuplen = sizeof(HeapTupleData) + tuple->t_len;

    /* send one byte flag */
    pq_sendint(&header, TupleType, sizeof(char));

    /* send content length, sizeof(relid) + sizeof(contentid) + tuple */
    int32 len = sizeof(Oid) + sizeof(int32) + tuplen;
    pq_sendint(&header, len, sizeof(int32));

    /* send relid */
    pq_sendint(&header, relid, sizeof(Oid));
    pq_sendint(&header, contentid, sizeof(int32));

    WriteData(cxt, header.data, header.len);
    WriteData(cxt, (const char *) tuple, sizeof(HeapTupleData));
    WriteData(cxt, (const char *) tuple->t_data, tuple->t_len);

    pfree(header.data);

    if (Debug_querycontext_print_tuple)
    {
        elog(LOG, "query context dispatching tuple for relation: %s, "
        "total bytes: %d", relname, (int)(len + sizeof(char) + sizeof(int32)));
    }
}

/*
 * collect tablespace location.
 */
void
AddTablespaceLocationToContextInfo(QueryContextInfo *cxt, Oid tspoid,
        const char *fmt)
{
    Assert(NULL != cxt);

    StringInfoData header;
    initStringInfo(&header);

    int len = strlen(fmt);

    pq_sendint(&header, TablespaceLocation, sizeof(char));
    pq_sendint(&header, sizeof(Oid) + len + 1, sizeof(int32));
    pq_sendint(&header, tspoid, sizeof(Oid));

    WriteData(cxt, header.data, header.len);
    WriteData(cxt, fmt, len + 1);

    pfree(header.data);
}

static bool
alreadyAddedForDispatching(HTAB *rels, Oid objid, QueryContextDispatchingObjType type) {
    bool found = false;

    Assert(NULL != rels);

    QueryContextDispatchingHashItem item;

    item.objid = objid;
    item.type = type;

    hash_search(rels, &item, HASH_ENTER, &found);

    return found;
}


/*
 * collect pg_namespace tuples for oid.
 * add them to in-memory heap table for dispatcher
 */
void
prepareDispatchedCatalogNamespace(QueryContextInfo *cxt, Oid namespace, HTAB *htab)
{
    HeapTuple tuple;

    if (namespace == PG_CATALOG_NAMESPACE || namespace == PG_TOAST_NAMESPACE
            || namespace == PG_BITMAPINDEX_NAMESPACE
            || namespace == PG_PUBLIC_NAMESPACE
            || namespace == PG_AOSEGMENT_NAMESPACE)
        return;

    if (alreadyAddedForDispatching(htab, namespace, NamespaceType))
        return;

    tuple = SearchSysCache(NAMESPACEOID, ObjectIdGetDatum(namespace), 0, 0, 0);

    if (!HeapTupleIsValid(tuple))
        elog(ERROR, "cache lookup failed for namespace %u", namespace);

    AddTupleToContextInfo(cxt, NamespaceRelationId, "pg_namespace", tuple,
            MASTER_CONTENT_ID);

    ReleaseSysCache(tuple);
}

/*
 * collect pg_tablespace tuples for oid.
 * add them to in-memory heap table for dispatcher
 */
void
prepareDispatchedCatalogTablespace(QueryContextInfo *cxt, Oid tablespace, HTAB *htab)
{

    char *path = NULL, *pos;
    char location[FilespaceLocationBlankPaddedWithNullTermLen];

    if (IsBuiltinTablespace(tablespace) || tablespace == InvalidOid )
        return;

    if (alreadyAddedForDispatching(htab, tablespace, TablespaceType))
        return;

    /*
     * here we get the seg0' s tablespace location since
     * master's tablespace location is special in gpsql.
     */
    Assert(GpIdentity.numsegments >=1);
    GetFilespacePathForTablespace(tablespace, 0, &path);

    Assert(NULL != path);
    Assert(strlen(path) < FilespaceLocationBlankPaddedWithNullTermLen);

    pos = path + strlen(path) - 1;
    Assert('0' == *pos);

    *pos = '\0';
    snprintf(location, FilespaceLocationBlankPaddedWithNullTermLen,
            "%s%s", path, "%d");

    pfree(path);

    AddTablespaceLocationToContextInfo(cxt, tablespace, location);
}

/*
 * to check if a type is a composite type,
 * dispatching type info if it is.
 */
static void
prepareDispatchedCatalogCompositeType(QueryContextInfo *cxt,
        Oid typeid, HTAB *htab)
{
    HeapTuple tuple;

    Form_pg_type attr;

    Assert(OidIsValid(typeid));

    tuple = SearchSysCache(TYPEOID, ObjectIdGetDatum(typeid), 0, 0, 0);

    if (!HeapTupleIsValid(tuple))
        return;

    attr = (Form_pg_type) GETSTRUCT(tuple);

    if (attr->typtype == TYPTYPE_COMPOSITE)
        prepareDispatchedCatalogSingleRelation(cxt, attr->typrelid, FALSE, 0, htab);

    ReleaseSysCache(tuple);
}

/*
 * collect relation's attribute info for dispatching
 */
static void
prepareDispatchedCatalogAttribute(QueryContextInfo *cxt,
        Oid reloid, HTAB *htab)
{
    Relation rel;
    HeapTuple attrtuple;

    ScanKeyData skey;
    SysScanDesc scandesc;

    Form_pg_attribute attr;

    rel = heap_open(AttributeRelationId, AccessShareLock);
    Assert(NULL != rel);

    ScanKeyInit(&skey, Anum_pg_attribute_attrelid, BTEqualStrategyNumber,
            F_OIDEQ, ObjectIdGetDatum(reloid));
    scandesc = systable_beginscan(rel, InvalidOid, FALSE, SnapshotNow, 1,
            &skey);
    attrtuple = systable_getnext(scandesc);
    while (HeapTupleIsValid(attrtuple))
    {
        attr = (Form_pg_attribute) GETSTRUCT(attrtuple);

        if (attr->attnum > 0)
        {
            /* check attribute type */
            if (OidIsValid(attr->atttypid))
                prepareDispatchedCatalogCompositeType(cxt, attr->atttypid, htab);
        }

        AddTupleToContextInfo(cxt, AttributeRelationId, "pg_attribute",
                attrtuple, MASTER_CONTENT_ID);

        attrtuple = systable_getnext(scandesc);
    }
    systable_endscan(scandesc);

    heap_close(rel, AccessShareLock);
}

/*
 * collect relation's defalut values for dispatching
 */
static void
prepareDispatchedCatalogAttributeDefault(QueryContextInfo *cxt,
        Oid reloid, HTAB *htab)
{
    Relation rel;
    Datum value;
    HeapTuple attrdeftuple;

    ScanKeyData skey;
    SysScanDesc scandesc;

    rel = heap_open(AttrDefaultRelationId, AccessShareLock);

    ScanKeyInit(&skey, Anum_pg_attrdef_adrelid, BTEqualStrategyNumber, F_OIDEQ,
            ObjectIdGetDatum(reloid));
    scandesc = systable_beginscan(rel, InvalidOid, FALSE, SnapshotNow, 1,
            &skey);
    attrdeftuple = systable_getnext(scandesc);
    while (HeapTupleIsValid(attrdeftuple))
    {
        /*
         * check for sequence
         */
        Expr *expr = NULL;
        bool isNull;

        value = fastgetattr(attrdeftuple, Anum_pg_attrdef_adbin,
                RelationGetDescr(rel), &isNull);

        Assert(FALSE == isNull);

        expr = stringToNode(TextDatumGetCString(value));
        if (expr)
        {
            prepareDispatchedCatalogFunctionExpr(cxt, expr, htab);
            pfree(expr);
        }

        AddTupleToContextInfo(cxt, AttrDefaultRelationId, "pg_attrdef",
                attrdeftuple, MASTER_CONTENT_ID);
        attrdeftuple = systable_getnext(scandesc);
    }
    systable_endscan(scandesc);

    heap_close(rel, AccessShareLock);

}

/*
 * collect relation's type info for dispatching
 */
static void
prepareDispatchedCatalogTypeByRelation(QueryContextInfo *cxt,
        Oid relid, HeapTuple classtuple, HTAB *htab)
{
    Datum typeid;
    HeapTuple typetuple;

    typeid = SysCacheGetAttr(RELOID, classtuple, Anum_pg_class_reltype, NULL );

    if (InvalidOid == DatumGetObjectId(typeid))
        elog(ERROR, "reltype in pg_class for %u is invalid", relid);

    typetuple = SearchSysCache(TYPEOID, typeid, 0, 0, 0);

    AddTupleToContextInfo(cxt, TypeRelationId, "pg_type", typetuple,
            MASTER_CONTENT_ID);

    /*
     * TODO:
     * 1) to check typelem for array type
     * 2) to check referenced tuple in pg_proc
     * 3) to check text type attributes
     */
    ReleaseSysCache(typetuple);
}

/*
 * collect pg_constraint for metadata dispatching
 */
static void
prepareDispatchedCatalogConstraint(QueryContextInfo *cxt,
        Oid relid)
{
    Relation rel;

    ScanKeyData skey;
    SysScanDesc scandesc;

    HeapTuple constuple;

    rel = heap_open(ConstraintRelationId, AccessShareLock);

    ScanKeyInit(&skey, Anum_pg_constraint_conrelid, BTEqualStrategyNumber,
            F_OIDEQ, ObjectIdGetDatum(relid));
    scandesc = systable_beginscan(rel, InvalidOid, FALSE, SnapshotNow, 1,
            &skey);
    constuple = systable_getnext(scandesc);
    while (constuple)
    {
        AddTupleToContextInfo(cxt, ConstraintRelationId, "pg_constraint",
                constuple, MASTER_CONTENT_ID);
        constuple = systable_getnext(scandesc);
    }
    systable_endscan(scandesc);

    heap_close(rel, AccessShareLock);
}

/*
 * collect pg_class/pg_type/pg_attribute tuples for oid.
 * add them to in-memory heap table for dispatcher.
 */
void
prepareDispatchedCatalogSingleRelation(QueryContextInfo *cxt, Oid relid,
        bool forInsert, int32 segno, HTAB *htab)
{
    HeapTuple classtuple;

    Datum namespace, tablespace, toastrelid, relkind, relstorage, relname;

    Assert(relid != InvalidOid);

    /*
     * buildin object, dispatch nothing
     */
    if (relid < FirstNormalObjectId)
        return;

    if (alreadyAddedForDispatching(htab, relid, RelationType))
        return;

    /* find relid in pg_class */
    classtuple = SearchSysCache(RELOID, ObjectIdGetDatum(relid), 0, 0, 0);

    if (!HeapTupleIsValid(classtuple))
        elog(ERROR, "cache lookup failed for relation %u", relid);

    if (Debug_querycontext_print)
    {
        Form_pg_class tup = (Form_pg_class) GETSTRUCT(classtuple);

        elog(LOG, "Query Context: prepare object: relid = %u, relname = %s, "
                "relnamespace = %u, relfilenode = %u, relkind = %c",
                relid, tup->relname.data, tup->relnamespace,
                tup->relfilenode, tup->relkind);
    }

    AddTupleToContextInfo(cxt, RelationRelationId, "pg_class", classtuple,
            MASTER_CONTENT_ID);

    /* collect pg_namespace info */
    namespace = SysCacheGetAttr(RELOID, classtuple, Anum_pg_class_relnamespace,
            NULL );

    if (InvalidOid == DatumGetObjectId(namespace))
        elog(ERROR, "relnamespace field in pg_class of %u is invalid", relid);

    prepareDispatchedCatalogNamespace(cxt, DatumGetObjectId(namespace), htab);

    /* collect pg_tablespace info */
    tablespace = SysCacheGetAttr(RELOID, classtuple,
            Anum_pg_class_reltablespace, NULL );

    /* collect toast info */
    toastrelid = SysCacheGetAttr(RELOID, classtuple,
            Anum_pg_class_reltoastrelid, NULL );

    if (InvalidOid != DatumGetObjectId(toastrelid))
    {
        /*TODO*/
        Insist(!"cannot handle toast table right now!");
    }

    /* collect pg_type info */
    prepareDispatchedCatalogTypeByRelation(cxt, relid, classtuple, htab);

    /* collect pg_attribute info */
    prepareDispatchedCatalogAttribute(cxt, relid, htab);

    /* collect pg_attrdef info */
    prepareDispatchedCatalogAttributeDefault(cxt, relid, htab);

    /* collect pg_constraint info */
    prepareDispatchedCatalogConstraint(cxt, relid);

    relkind = SysCacheGetAttr(RELOID, classtuple, Anum_pg_class_relkind, NULL);

    relstorage = SysCacheGetAttr(RELOID, classtuple, Anum_pg_class_relstorage,
            NULL);

    relname = SysCacheGetAttr(RELOID, classtuple, Anum_pg_class_relname,
            NULL);

	/* The dfs tablespace oid must be dispatched. */
	if (DatumGetObjectId(tablespace) == InvalidOid && relstorage_is_ao(relstorage))
		prepareDispatchedCatalogTablespace(cxt, get_database_dts(MyDatabaseId), htab);
	else
		prepareDispatchedCatalogTablespace(cxt, DatumGetObjectId(tablespace), htab);

	switch (DatumGetChar(relstorage))
	{
	case RELSTORAGE_AOROWS:
	case RELSTORAGE_AOCOLS:
		prepareDispatchedCatalogForAoCo(cxt, relid, forInsert, segno, htab);
		break;
	case RELSTORAGE_EXTERNAL:
	    prepareDispatchedCatalogExternalTable(cxt, relid);
	    break;
	case RELSTORAGE_VIRTUAL:
	    break;
	case RELSTORAGE_HEAP:
	    /* support catalog tables (gp_dist_random et al.) */
	    break;

	case RELSTORAGE_FOREIGN:
	    /* TODO */
	    elog(ERROR, "not implemented relstorage: %c, relname: %s",
	            DatumGetChar(relstorage), DatumGetCString(relname));
	    break;
	default:
	    Insist(!"never get here");
	    break;
	}

    ReleaseSysCache(classtuple);
}

/*
 * parse fast_sequence for dispatch
 */
static void
prepareDispatchedCatalogFastSequence(QueryContextInfo *cxt,
        Oid objid, int64 objmod)
{

    SysScanDesc scanDesc;
    HeapTuple tuple;
    Datum contentid;

    Relation fast_seq_rel;

    fast_seq_rel = heap_open(FastSequenceRelationId, AccessShareLock);

    ScanKeyData scanKeys[2];

    ScanKeyInit(&scanKeys[0], Anum_gp_fastsequence_objid, BTEqualStrategyNumber,
            F_OIDEQ, ObjectIdGetDatum(objid));

    ScanKeyInit(&scanKeys[1], Anum_gp_fastsequence_objmod,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(objmod));

    scanDesc = systable_beginscan(fast_seq_rel, InvalidOid, FALSE,
            SnapshotNow, 2, scanKeys);

    while (HeapTupleIsValid(tuple = systable_getnext(scanDesc)))
    {
        contentid = heap_getattr(tuple, Anum_gp_fastsequence_contentid,
                RelationGetDescr(fast_seq_rel), NULL );

        AddTupleToContextInfo(cxt, FastSequenceRelationId, "gp_fastsequence",
                tuple, DatumGetInt32(contentid));
    }

    systable_endscan(scanDesc);

    heap_close(fast_seq_rel, AccessShareLock);
}

/*
 * parse pg_appendonly for dispatch
 */
static void
prepareDispatchedCatalogGpAppendOnly(QueryContextInfo *cxt,
        Oid relid, Oid *segrelid, Oid *segidxid)
{
    Relation pg_appendonly_rel;
    TupleDesc pg_appendonly_dsc;
    HeapTuple tuple;
    ScanKeyData key;
    SysScanDesc aoscan;

    Datum temp;

    /*
     * Check the pg_appendonly relation to be certain the ao table
     * is there.
     */
    pg_appendonly_rel = heap_open(AppendOnlyRelationId, AccessShareLock);
    pg_appendonly_dsc = RelationGetDescr(pg_appendonly_rel);

    ScanKeyInit(&key, Anum_pg_appendonly_relid, BTEqualStrategyNumber, F_OIDEQ,
            ObjectIdGetDatum(relid));

    aoscan = systable_beginscan(pg_appendonly_rel, AppendOnlyRelidIndexId, TRUE,
            SnapshotNow, 1, &key);

    tuple = systable_getnext(aoscan);

    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("missing pg_appendonly"
                " entry for relation \"%s\"", get_rel_name(relid))));

    AddTupleToContextInfo(cxt, AppendOnlyRelationId, "pg_appendonly", tuple,
            MASTER_CONTENT_ID);

    temp = heap_getattr(tuple, Anum_pg_appendonly_segrelid, pg_appendonly_dsc,
            NULL );

    *segrelid = DatumGetObjectId(temp);

    temp = heap_getattr(tuple, Anum_pg_appendonly_segidxid, pg_appendonly_dsc,
            NULL );

    *segidxid = DatumGetObjectId(temp);

    if (Debug_querycontext_print)
    {
        elog(LOG, "Query Context: prepare pg_appendonly for rel = %u, "
                "segrelid = %u", relid, *segrelid);
    }

    /* Finish up scan and close pg_appendonly catalog. */
    systable_endscan(aoscan);

    heap_close(pg_appendonly_rel, AccessShareLock);

}

/*
 * add pg_aoseg_XXX's content
 */
static void
prepareDispatchedCatalogAoSegfile(QueryContextInfo *cxt,
        Relation rel, Oid segrelid, bool forInsert, int32 segno)
{
    Relation ao_seg_rel;
    HeapTuple pg_aoseg_tuple;

    ScanKeyData key;

    bool empty = true;

    SysScanDesc aosegScanDesc;

    ao_seg_rel = heap_open(segrelid, AccessShareLock);

    if (forInsert)
    {
        ScanKeyInit(&key, Anum_pg_aoseg_segno, BTEqualStrategyNumber,
                F_INT4EQ, segno);

        aosegScanDesc = systable_beginscan(ao_seg_rel, InvalidOid, FALSE,
                SnapshotNow, 1, &key);
    }
    else
    {
        aosegScanDesc = systable_beginscan(ao_seg_rel, InvalidOid, FALSE,
                SnapshotNow, 0, NULL );
    }

    while (HeapTupleIsValid(pg_aoseg_tuple = systable_getnext(aosegScanDesc)))
    {
        Datum segno, contentid;

        empty = false;

        segno = heap_getattr(pg_aoseg_tuple, 1, RelationGetDescr(ao_seg_rel),
                NULL );

        if (RelationIsAoRows(rel))
        {
            contentid = heap_getattr(pg_aoseg_tuple, Anum_pg_aoseg_content,
                    RelationGetDescr(ao_seg_rel), NULL );
        }
        else
        {
            Assert(RelationIsAoCols(rel));

            contentid = heap_getattr(pg_aoseg_tuple,
                    Anum_pg_aocs_content, RelationGetDescr(ao_seg_rel),
                    NULL );
        }

        AddTupleToContextInfo(cxt, segrelid,
                RelationGetRelationName(ao_seg_rel), pg_aoseg_tuple,
                DatumGetInt32(contentid));
    }

    systable_endscan(aosegScanDesc);

    heap_close(ao_seg_rel, AccessShareLock);

    if (empty)
        AddEmptyTableFlagToContextInfo(cxt, segrelid);
}

/*
 * parse AO/CO relation range table and collect metadata used for QE
 * add them to in-memory heap table for dispatcher.
 */
static void
prepareDispatchedCatalogForAoCo(QueryContextInfo *cxt, Oid relid,
        bool forInsert, int32 segno, HTAB *htab)
{

    Relation rel;
    Oid segrelid, segidxid;

    rel = heap_open(relid, AccessShareLock);

    Assert((RelationIsAoRows(rel) || RelationIsAoCols(rel)));

    /*
     * add tuple in pg_appendonly
     */
    prepareDispatchedCatalogGpAppendOnly(cxt, RelationGetRelid(rel), &segrelid,
            &segidxid);

    Assert(segrelid != InvalidOid);

    /*
     * add pg_aoseg_XXX's metadata
     */
    prepareDispatchedCatalogSingleRelation(cxt, segrelid, FALSE, 0, htab);

    /*
     * add gp_aoseg_xxx's content
     */
    prepareDispatchedCatalogAoSegfile(cxt, rel, segrelid, forInsert, segno);

    /*
     * add gp_fastsequence
     */
    if (forInsert)
        prepareDispatchedCatalogFastSequence(cxt, segrelid, segno);

    /*TODO index for ao/cs*/

    heap_close(rel, AccessShareLock);
}

/*
 * parse AO/CO relation range table and collect metadata used for QE
 * add them to in-memory heap table for dispatcher.
 */
static void
prepareDispatchedCatalogExternalTable(QueryContextInfo *cxt,
        Oid relid)
{
    Relation pg_exttable_rel;
    TupleDesc pg_exttable_dsc;
    HeapTuple tuple;
    ScanKeyData key;
    SysScanDesc extscan;

    Relation rel;

    rel = heap_open(relid, AccessShareLock);

    pg_exttable_rel = heap_open(ExtTableRelationId, RowExclusiveLock);
    pg_exttable_dsc = RelationGetDescr(pg_exttable_rel);

    ScanKeyInit(&key, Anum_pg_exttable_reloid, BTEqualStrategyNumber, F_OIDEQ,
            ObjectIdGetDatum(rel->rd_id));

    extscan = systable_beginscan(pg_exttable_rel, ExtTableReloidIndexId, true,
            SnapshotNow, 1, &key);

    tuple = systable_getnext(extscan);
    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("missing "
                "pg_exttable entry for relation \"%s\"", RelationGetRelationName(rel))));

    AddTupleToContextInfo(cxt, ExtTableRelationId, "pg_exttable", tuple,
            MASTER_CONTENT_ID);

    systable_endscan(extscan);

    heap_close(pg_exttable_rel, RowExclusiveLock);

    heap_close(rel, AccessShareLock);
}

/*
 * recursively scan the expression chain and collect function information
 */
static void
prepareDispatchedCatalogFunctionExpr(QueryContextInfo *cxt, Expr *expr, HTAB *htab)
{
    Assert(NULL != cxt && NULL != expr && NULL != htab);

    if (!IsA(expr, FuncExpr))
        return;

    FuncExpr *func = (FuncExpr *) expr;

    switch (func->funcid)
    {
    case NEXTVAL_FUNC_OID:
    {
        Const *arg;
        Oid seqoid;

        arg = (Const *) linitial(func->args);
        if (arg->xpr.type == T_Const && arg->consttype == REGCLASSOID)
        {
            seqoid = DatumGetObjectId(arg->constvalue);
            
            /* 
             * aclchecks for nextval are done on the segments. But in GPSQL 
             * we are executing as bootstrap user on the segments. So any aclchecks
             * on segments defeats the purpose. 
             * Do the aclchecks on the master, prior to dispatch
             */
            if (pg_class_aclcheck(seqoid, GetUserId(), ACL_UPDATE) != ACLCHECK_OK)
                ereport(ERROR,
                                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                                 errmsg("permission denied for sequence %s",
                                                get_rel_name(seqoid))));
            prepareDispatchedCatalogSingleRelation(cxt, seqoid, FALSE, 0, htab);
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_CDB_FEATURE_NOT_YET),
                            errmsg("Non const argument in function \"NEXTVAL\" is not support yet in GPSQL.")));
        }


    }
        break;
    default:
    {
        ListCell *tlist;
        Expr *next;
        foreach(tlist, func->args)
        {
            next = (Expr *) lfirst(tlist);
            prepareDispatchedCatalogFunctionExpr(cxt, next, htab);
        }
    }
        break;
    }
}

static int32
findSegnofromMap(Oid relid, List *segnoMaps)
{
	ListCell *relid_to_segno;
	int32 mysegno = InvalidFileSegNumber;

	foreach(relid_to_segno, segnoMaps)
	{
		SegfileMapNode *n = (SegfileMapNode *) lfirst(relid_to_segno);

		if (n->relid == relid)
		{
			Assert(n->segno != InvalidFileSegNumber);
			mysegno = n->segno;
			break;
		}
	}
	return mysegno;
}

/*
 * parse Relation and collect metadata used for QE add them to in-memory heap
 * table for dispatcher.
 */
void
prepareDispatchedCatalogRelation(QueryContextInfo *cxt, Oid relid,
        bool forInsert, List *segnoMaps, HTAB *htab)
{
    List *children = NIL;
    ListCell *child;


    if (rel_is_partitioned(relid))
    {
        children = find_all_inheritors(relid);
        foreach(child, children)
        {
            Oid myrelid = lfirst_oid(child);
            if (forInsert)
            {
				int32 mysegno = findSegnofromMap(relid, segnoMaps);
                Assert(mysegno != InvalidFileSegNumber);
                prepareDispatchedCatalogSingleRelation(cxt, myrelid, TRUE,
                        mysegno, htab);

            }
            else
            {
                prepareDispatchedCatalogSingleRelation(cxt, myrelid, FALSE, 0,
                        htab);
            }
        }
        list_free(children);
    }
    else
    {
        if (forInsert && NULL != segnoMaps)
        {
            int32 mysegno = findSegnofromMap(relid, segnoMaps);

            Assert(mysegno != InvalidFileSegNumber);
            prepareDispatchedCatalogSingleRelation(cxt, relid, TRUE, mysegno,
                    htab);
        }
        else
        {
            prepareDispatchedCatalogSingleRelation(cxt, relid, FALSE, 0, htab);
        }
    }
}

/*
 * parse target list to handle function.
 */
void
prepareDispatchedCatalogTargets(QueryContextInfo *cxt, List *targets, HTAB *htab)
{
    ListCell *lc;

    Assert(NULL != targets);
    Assert(NULL != cxt && NULL != htab);

    foreach(lc, targets)
    {
        TargetEntry *te = lfirst(lc);

        if (te->resjunk)
            continue;

        if (te->expr)
            prepareDispatchedCatalogFunctionExpr(cxt, te->expr, htab);
    }
}

/*
 * parse plan for some functions in plan's targets.
 */
void
prepareDispatchedCatalogPlan(QueryContextInfo *cxt, Plan *plan, HTAB *htab)
{
    if (!plan)
        return;

    Assert(NULL != cxt && NULL != htab);

    if (plan->targetlist)
        prepareDispatchedCatalogTargets(cxt, plan->targetlist, htab);

    prepareDispatchedCatalogPlan(cxt, plan->lefttree, htab);

    prepareDispatchedCatalogPlan(cxt, plan->righttree, htab);

}

/*
 * parse range table and collect metadata used for QE,
 * add them to in-memory heap table for dispatcher.
 */
void
prepareDispatchedCatalog(QueryContextInfo *cxt, List *rtable, HTAB *htab)
{
    ListCell *lc;

    Assert(NULL != rtable);
    Assert(NULL != cxt && NULL != htab);

    foreach(lc, rtable)
    {
        RangeTblEntry *rte = lfirst(lc);
        switch (rte->rtekind)
        {
        case RTE_RELATION:             /*ordinary relation reference */
            prepareDispatchedCatalogRelation(cxt, rte->relid, FALSE, NULL, htab);
            break;

        case RTE_FUNCTION:             /*function in FROM */
            if (rte->funcexpr)
                    prepareDispatchedCatalogFunctionExpr(cxt, (Expr *)rte->funcexpr, htab);
            break;

        case RTE_SUBQUERY:             /*subquery in FROM */
            if (rte->subquery)
            {
                List *sub_rtable = rte->subquery->rtable;
                List *sub_targets = rte->subquery->targetList;
                if (sub_rtable)
                    prepareDispatchedCatalog(cxt, sub_rtable, htab);
                if (sub_targets)
                    prepareDispatchedCatalogTargets(cxt, sub_targets, htab);
            }
            break;

        case RTE_VALUES:               /*VALUES (<exprlist>), (<exprlist>): ... */
            if (rte->values_lists)
            {
                ListCell *lc;

                foreach(lc, rte->values_lists)
                {
                    List *exprs = lfirst(lc);

                    if (exprs)
                    {
                        ListCell *cell;

                        foreach(cell, rte->values_lists)
                        {
                            Expr *expr = lfirst(cell);

                            prepareDispatchedCatalogFunctionExpr(cxt, expr,
                                    htab);
                        }
                    }
                }
            }
            break;

        case RTE_JOIN:                 /*join */
        case RTE_VOID:                 /*CDB: deleted RTE */
            break;

        case RTE_SPECIAL:              /*special rule relation (NEW or OLD) */
        case RTE_CTE:                  /*CommonTableExpr in FROM */
        case RTE_TABLEFUNCTION:        /*CDB: Functions over multiset input */
            /*TODO */
            elog(ERROR, "not implemented");
            break;
        default:
            Insist(!"never get here");
            break;
        }
    }
}

/*
 * create a hash table used to filter out the duplicated table
 */
HTAB *
createPrepareDispatchedCatalogRelationDisctinctHashTable(void)
{
    HASHCTL info;
    HTAB *rels = NULL;

    MemSet(&info, 0, sizeof(info));

    info.hash = tag_hash;
    info.keysize = sizeof(QueryContextDispatchingHashItem);
    info.entrysize = sizeof(QueryContextDispatchingHashItem);

    rels = hash_create("all relations", 10, &info, HASH_FUNCTION | HASH_ELEM);

    return rels;
}

QueryContextDispatchingSendBack
CreateQueryContextDispatchingSendBack(int nfile)
{
	QueryContextDispatchingSendBack rc =
			palloc0(sizeof(struct QueryContextDispatchingSendBackData));
	rc->numfiles = nfile;
	rc->eof = palloc0(nfile * sizeof(int64));
	rc->uncompressed_eof = palloc0(nfile * sizeof(int64));

	return rc;
}

void
DropQueryContextDispatchingSendBack(QueryContextDispatchingSendBack sendback)
{
	if (sendback)
	{
		if (sendback->eof)
			pfree(sendback->eof);
		if (sendback->uncompressed_eof)
			pfree(sendback->uncompressed_eof);
		pfree(sendback);
	}
}


void
UpdateCatalogModifiedOnSegments(QueryContextDispatchingSendBack sendback)
{
	Assert(NULL != sendback);

	AppendOnlyEntry *aoEntry = GetAppendOnlyEntry(sendback->relid, SnapshotNow);
	Assert(aoEntry != NULL);

	Relation rel = heap_open(sendback->relid, AccessShareLock);
	if (RelationIsAoCols(rel))
	{
		Insist(sendback->numfiles == rel->rd_att->natts);
		UpdateAOCSFileSegInfo(rel, aoEntry, sendback->segno,
				sendback->insertCount, sendback->varblock, sendback->eof,
				sendback->uncompressed_eof, sendback->contentid);
	}
	else if (RelationIsAoRows(rel))
	{
		Insist(sendback->numfiles == 1);
		UpdateFileSegInfo(rel, aoEntry, sendback->segno,
				sendback->eof[0], sendback->uncompressed_eof[0], sendback->insertCount,
				sendback->varblock, sendback->contentid);
	}
	else
	{
		Insist(!"bug, should no get here.");
	}

	ItemPointerData tid;

	InsertFastSequenceEntry(aoEntry->segrelid, sendback->segno,
			sendback->nextFastSequence, sendback->contentid, &tid);

	heap_close(rel, AccessShareLock);
}

StringInfo
PreSendbackChangedCatalog(int aocount)
{
	StringInfo buf = makeStringInfo();

	pq_beginmessage(buf, 'h');

	/*
	 * 1, send contentid
	 */
	pq_sendint(buf, GpIdentity.segindex, 4);

	/*
	 * 2, send number of relations
	 */
	pq_sendint(buf, aocount, 4);

	return buf;
}

void
AddSendbackChangedCatalogContent(StringInfo buf,
		QueryContextDispatchingSendBack sendback)
{
	int i;

	/*
	 * 3, send relation id
	 */
	pq_sendint(buf, sendback->relid, 4);
	/*
	 * 4, send insertCount
	 */
	pq_sendint64(buf, sendback->insertCount);

	/*
	 * 5, send segment file no.
	 */
	pq_sendint(buf, sendback->segno, 4);
	/*
	 * 6, send varblock count
	 */
	pq_sendint64(buf, sendback->varblock);

	/*
	 * 7, send number of files
	 */
	pq_sendint(buf, sendback->numfiles, 4);

	for (i = 0; i < sendback->numfiles; ++i)
	{
		/*
		 * 8, send eof
		 */
		pq_sendint64(buf, sendback->eof[i]);
		/*
		 * 9 send uncompressed eof
		 */
		pq_sendint64(buf, sendback->uncompressed_eof[i]);
	}

	/*
	 * 10, send next fast sequence.
	 */
	pq_sendint64(buf, sendback->nextFastSequence);
}


void
FinishSendbackChangedCatalog(StringInfo buf)
{
	pq_endmessage(buf);
	pfree(buf);
}
