/*-------------------------------------------------------------------------
 *
 * pxfnamenode.c
 *	  Functions for getting info from the PXF master.
 *	  Used by hd_work_mgr and gpbridgeapi
 *
 * Copyright (c) 2007-2013, Pivotal inc
 *
 *-------------------------------------------------------------------------
 */
#include <json/json.h>
#include "access/pxfmasterapi.h"

static List* parse_datanodes_response(List *rest_srvrs, StringInfo rest_buf);
static PxfStatsElem* parse_get_stats_response(StringInfo rest_buf);
static List* parse_get_fragments_response(List* fragments, StringInfo rest_buf);

/*
 * Wrapper for libchurl
 */
void process_request(ClientContext* client_context, char *uri)
{
	size_t n = 0;

	print_http_headers(client_context->http_headers);
	client_context->handle = churl_init_download(uri, client_context->http_headers);
	memset(client_context->chunk_buf, 0, RAW_BUF_SIZE);
	resetStringInfo(&(client_context->the_rest_buf));

	while ((n = churl_read(client_context->handle, client_context->chunk_buf, sizeof(client_context->chunk_buf))) != 0)
	{
		appendBinaryStringInfo(&(client_context->the_rest_buf), client_context->chunk_buf, n);
		memset(client_context->chunk_buf, 0, RAW_BUF_SIZE);
	}

	churl_cleanup(client_context->handle);
}

/*
 * Obtain the datanode REST servers host/port data
 */
static List*
parse_datanodes_response(List *rest_srvrs, StringInfo rest_buf)
{
	struct json_object *whole = json_tokener_parse(rest_buf->data);
	struct json_object *nodes = json_object_object_get(whole, "regions");
	int length = json_object_array_length(nodes);

	/* obtain host/port data for the REST server running on each HDFS data node */
	for (int i = 0; i < length; i++)
	{
		DataNodeRestSrv* srv = (DataNodeRestSrv*)palloc(sizeof(DataNodeRestSrv));

		struct json_object *js_node = json_object_array_get_idx(nodes, i);
		struct json_object *js_host = json_object_object_get(js_node, "host");
		srv->host = pstrdup(json_object_get_string(js_host));
		struct json_object *js_port = json_object_object_get(js_node, "port");
		srv->port = json_object_get_int(js_port);

		rest_srvrs = lappend(rest_srvrs, srv);
	}

	return rest_srvrs;
}

/*
 * Get host/port data for the REST servers running on each datanode
 */
List*
get_datanode_rest_servers(GPHDUri *hadoop_uri, ClientContext* client_context)
{
	List* rest_srvrs_list = NIL;
	StringInfoData get_srvrs_uri;
	initStringInfo(&get_srvrs_uri);

	Assert(hadoop_uri->host != NULL && hadoop_uri->port != NULL);

	appendStringInfo(&get_srvrs_uri, "http://%s:%s/%s/%s/HadoopCluster/getNodesInfo",
											hadoop_uri->host,
											hadoop_uri->port,
											GPDB_REST_PREFIX,
											PXF_VERSION);

	process_request(client_context, get_srvrs_uri.data);

	/*
	 * the curl client finished the communication. The response from
	 * the backend lies at rest_buf->data
	 */
	rest_srvrs_list = parse_datanodes_response(rest_srvrs_list, &(client_context->the_rest_buf));
	pfree(get_srvrs_uri.data);

	return rest_srvrs_list;
}

void
free_datanode_rest_servers(List *srvrs)
{
	ListCell *srv_cell = NULL;
	foreach(srv_cell, srvrs)
	{
		DataNodeRestSrv* srv = (DataNodeRestSrv*)lfirst(srv_cell);
		free_datanode_rest_server(srv);
	}
	list_free(srvrs);
}

void free_datanode_rest_server(DataNodeRestSrv* srv)
{
	if (srv->host)
		pfree(srv->host);
	pfree(srv);
}

/*
 * Fetch the statistics from the PXF service
 */
PxfStatsElem *get_data_statistics(GPHDUri* hadoop_uri,
										 ClientContext *cl_context,
										 StringInfo err_msg)
{
	StringInfoData request;
	initStringInfo(&request);

	/* construct the request */
	appendStringInfo(&request, "http://%s:%s/%s/%s/Analyzer/getEstimatedStats?path=%s",
					 hadoop_uri->host,
					 hadoop_uri->port,
					 GPDB_REST_PREFIX,
					 PXF_VERSION,
					 hadoop_uri->data);

	/* send the request. The response will exist in rest_buf.data */
	PG_TRY();
	{
		process_request(cl_context, request.data);
	}
	PG_CATCH();
	{
		/*
		 * communication problems with PXF service
		 * Statistics for a table can be done as part of an ANALYZE procedure on many tables,
		 * and we don't want to stop because of a communication error. So we catch the exception,
		 * append its error to err_msg, and return a NULL,
		 * which will force the the analyze code to use former calculated values or defaults.
		 */
		if (err_msg)
		{
			char* message = elog_message();
			if (message)
				appendStringInfo(err_msg, "%s", message);
			else
				appendStringInfo(err_msg, "Unknown error");

		}
		/* release error state */
		if (!elog_dismiss(DEBUG5))
			PG_RE_THROW(); /* hope to never get here! */

		return NULL;
	}
	PG_END_TRY();

	/* parse the JSON response and form a fragments list to return */
	return parse_get_stats_response(&(cl_context->the_rest_buf));
}

/*
 * Parse the json response from the PXF Fragmenter.getSize
 */
static PxfStatsElem *parse_get_stats_response(StringInfo rest_buf)
{
	PxfStatsElem* statsElem = (PxfStatsElem*)palloc0(sizeof(PxfStatsElem));
	struct json_object	*whole	= json_tokener_parse(rest_buf->data);
	struct json_object	*head	= json_object_object_get(whole, "PXFDataSourceStats");

	/* 0. block size */
	struct json_object *js_block_size = json_object_object_get(head, "blockSize");
	statsElem->blockSize = json_object_get_int(js_block_size);

	/* 1. number of blocks */
	struct json_object *js_num_blocks = json_object_object_get(head, "numberOfBlocks");
	statsElem->numBlocks = json_object_get_int(js_num_blocks);

	/* 2. number of tuples */
	struct json_object *js_num_tuples = json_object_object_get(head, "numberOfTuples");
	statsElem->numTuples = json_object_get_int(js_num_tuples);

	return statsElem;
}

/*
 * get_data_fragment_list
 *
 * 1. Request a list of fragments from the PXF Fragmenter class
 * that was specified in the pxf URL.
 *
 * 2. Process the returned list and create a list of DataFragment with it.
 */
List*
get_data_fragment_list(GPHDUri *hadoop_uri,
					   ClientContext *client_context)
{
	List *data_fragments = NIL;
	StringInfoData request;

	initStringInfo(&request);

	/* construct the request */
	appendStringInfo(&request, "http://%s:%s/%s/%s/Fragmenter/getFragments?path=%s",
								hadoop_uri->host,
								hadoop_uri->port,
								GPDB_REST_PREFIX,
								PXF_VERSION,
								hadoop_uri->data);

	/* send the request. The response will exist in rest_buf.data */
	process_request(client_context, request.data);

	/* parse the JSON response and form a fragments list to return */
	data_fragments = parse_get_fragments_response(data_fragments, &(client_context->the_rest_buf));

	return data_fragments;
}

/*
 * parse the response of the PXF Fragments call. An example:
 *
 * Request:
 * 		curl --header "X-GP-FRAGMENTER: HdfsDataFragmenter" "http://goldsa1mac.local:50070/gpdb/v2/Fragmenter/getFragments?path=demo" (demo is a directory)
 *
 * Response (left as a single line purposefully):
 * {"PXFFragments":[{"hosts":["isengoldsa1mac.corp.emc.com","isengoldsa1mac.corp.emc.com","isengoldsa1mac.corp.emc.com"],"sourceName":"text2.csv"},{"hosts":["isengoldsa1mac.corp.emc.com","isengoldsa1mac.corp.emc.com","isengoldsa1mac.corp.emc.com"],"sourceName":"text_data.csv"}]}
 */
static List*
parse_get_fragments_response(List *fragments, StringInfo rest_buf)
{
	struct json_object	*whole	= json_tokener_parse(rest_buf->data);
	struct json_object	*head	= json_object_object_get(whole, "PXFFragments");
	List* 				ret_frags = fragments;
	int 				length	= json_object_array_length(head);
    char *cur_source_name = NULL;
	int cur_index_count = 0;

	/* obtain split information from the block */
	for (int i = 0; i < length; i++)
	{
		struct json_object *js_fragment = json_object_array_get_idx(head, i);
		DataFragment* fragment = (DataFragment*)palloc0(sizeof(DataFragment));

		/* 0. source name */
		struct json_object *block_data = json_object_object_get(js_fragment, "sourceName");
		fragment->source_name = pstrdup(json_object_get_string(block_data));

		/* 1. fragment index, incremented per source name */
		if ((cur_source_name==NULL) || (strcmp(cur_source_name, fragment->source_name) != 0))
		{
			elog(FRAGDEBUG, "pxf: parse_get_fragments_response: "
						  "new source name %s, old name %s, init index counter %d",
						  fragment->source_name,
						  cur_source_name ? cur_source_name : "NONE",
						  cur_index_count);

			cur_index_count = 0;

			if (cur_source_name)
				pfree(cur_source_name);
			cur_source_name = pstrdup(fragment->source_name);
		}
		fragment->index = cur_index_count;
		++cur_index_count;

		/* 2. hosts - list of all machines that contain this fragment */
		struct json_object *js_fragment_hosts = json_object_object_get(js_fragment, "hosts");
		int num_hosts = json_object_array_length(js_fragment_hosts);

		for (int j = 0; j < num_hosts; j++)
		{
			FragmentLocation* floc = (FragmentLocation*)palloc(sizeof(FragmentLocation));
			struct json_object *loc = json_object_array_get_idx(js_fragment_hosts, j);

			floc->ip = pstrdup(json_object_get_string(loc));

			fragment->locations = lappend(fragment->locations, floc);
		}

		/* 3. userdata - additional user information */
		struct json_object *js_user_data = json_object_object_get(js_fragment, "userData");
		if (js_user_data)
			fragment->user_data = pstrdup(json_object_get_string(js_user_data));

		/*
		 * HD-2547:
		 * Ignore fragment if it doesn't contain any host locations,
		 * for example if the file is empty.
		 */
		if (fragment->locations)
			ret_frags = lappend(ret_frags, fragment);
		else
			free_fragment(fragment);

	}
	if (cur_source_name)
		pfree(cur_source_name);
	return ret_frags;
}

/*
 * release memory of a single fragment
 */
void free_fragment(DataFragment *fragment)
{
	ListCell *loc_cell = NULL;

	Assert(fragment != NULL);

	if (fragment->source_name)
		pfree(fragment->source_name);

	foreach(loc_cell, fragment->locations)
	{
		FragmentLocation* location = (FragmentLocation*)lfirst(loc_cell);

		if (location->ip)
			pfree(location->ip);
		pfree(location);
	}
	list_free(fragment->locations);

	if (fragment->user_data)
		pfree(fragment->user_data);
	pfree(fragment);
}
