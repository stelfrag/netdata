// SPDX-License-Identifier: GPL-3.0-or-later

#include "mcp-tools-alert-evidence.h"
#include "mcp-params.h"
#include "web/api/functions/function-alert-evidence.h"

// The tool runs the alert-evidence Function of a node (this agent by default; through streaming for a child) with
// the caller's own access, and returns its JSON answer as text.

void mcp_tool_get_alert_evidence_schema(BUFFER *buffer) {
    buffer_json_member_add_object(buffer, "inputSchema");
    buffer_json_member_add_string(buffer, "type", "object");
    buffer_json_member_add_string(buffer, "title", "Get the evidence for an alert transition");

    buffer_json_member_add_object(buffer, "properties");

    mcp_schema_add_string_param(
        buffer, "transition_id", "Alert transition id",
        "The transition_id of one alert transition, as returned by '" MCP_TOOL_LIST_ALERT_TRANSITIONS "'. "
        "Use a transition of the node that evaluated the alert: each agent keeps evidence for its own alerts.",
        NULL, true);

    mcp_schema_add_string_param(
        buffer, "node", "Node",
        "The node whose agent holds the evidence (hostname, machine GUID or node id). "
        "Use '" MCP_TOOL_LIST_NODES "' to discover nodes. Defaults to this agent.",
        NULL, false);

    mcp_schema_add_timeout(buffer, "timeout", "Query timeout",
        "Maximum time to wait for the evidence (in seconds)", 90, 10, 300, false);

    buffer_json_object_close(buffer); // properties

    buffer_json_member_add_array(buffer, "required");
    buffer_json_add_array_item_string(buffer, "transition_id");
    buffer_json_array_close(buffer);

    buffer_json_object_close(buffer); // inputSchema
}

MCP_RETURN_CODE mcp_tool_get_alert_evidence_execute(MCP_CLIENT *mcpc, struct json_object *params, MCP_REQUEST_ID id) {
    if(!mcpc || !mcpc->user_auth)
        return MCP_RC_ERROR;

    const char *transition = mcp_params_extract_string(params, "transition_id", NULL);
    nd_uuid_t uuid;
    if(!transition || uuid_parse(transition, uuid) != 0) {
        buffer_strcat(mcpc->error, "'transition_id' is required and must be a transition id (a UUID)");
        return MCP_RC_BAD_REQUEST;
    }

    int timeout = mcp_params_extract_timeout(params, "timeout", 90, 10, 300, mcpc->error);
    if(buffer_strlen(mcpc->error))
        return MCP_RC_BAD_REQUEST;

    RRDHOST *host = localhost;
    const char *node = mcp_params_extract_string(params, "node", NULL);
    if(node && *node) {
        host = rrdhost_find_by_hostname(node);
        if(!host) host = rrdhost_find_by_guid(node);
        if(!host) host = rrdhost_find_by_node_id(node);
        if(!host) {
            buffer_sprintf(mcpc->error, "Node not found: %s", node);
            return MCP_RC_NOT_FOUND;
        }
    }

    char cmd[128];
    char transition_str[UUID_STR_LEN];
    uuid_unparse_lower(uuid, transition_str);
    snprintfz(cmd, sizeof(cmd), FUNCTION_ALERT_EVIDENCE " transition:%s", transition_str);

    CLEAN_BUFFER *source = buffer_create(0, NULL);
    user_auth_to_source_buffer(mcpc->user_auth, source);
    buffer_strcat(source, ",modelcontextprotocol");

    CLEAN_BUFFER *result = buffer_create(0, NULL);
    int code = nrpc_call(&(struct nrpc_call_spec) {
        .owner = rrdhost_nrpc_owner(host),
        .result_wb = result,
        .cmd = cmd,
        .source = buffer_tostring(source),
        .user_access = mcpc->user_auth->access,
        .timeout_s = timeout,
        .wait = true,
        .allow_restricted = false,
    });

    if(code != HTTP_RESP_OK) {
        buffer_sprintf(mcpc->error, "alert-evidence on node '%s' answered %d (%s):\n```json\n%s\n```",
                       rrdhost_hostname(host), code, http_response_code2string(code), buffer_tostring(result));
        return code == HTTP_RESP_NOT_FOUND ? MCP_RC_NOT_FOUND : MCP_RC_ERROR;
    }

    mcp_init_success_result(mcpc, id);
    buffer_json_member_add_array(mcpc->result, "content");
    {
        buffer_json_add_array_item_object(mcpc->result);
        {
            buffer_json_member_add_string(mcpc->result, "type", "text");
            buffer_json_member_add_string(mcpc->result, "text", buffer_tostring(result));
        }
        buffer_json_object_close(mcpc->result);
    }
    buffer_json_array_close(mcpc->result);
    buffer_json_object_close(mcpc->result);
    buffer_json_finalize(mcpc->result);

    return MCP_RC_OK;
}
