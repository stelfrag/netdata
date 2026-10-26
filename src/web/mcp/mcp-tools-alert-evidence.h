// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_MCP_TOOLS_ALERT_EVIDENCE_H
#define NETDATA_MCP_TOOLS_ALERT_EVIDENCE_H

#include "mcp.h"

#define MCP_TOOL_GET_ALERT_EVIDENCE "get_alert_evidence"

void mcp_tool_get_alert_evidence_schema(BUFFER *buffer);
MCP_RETURN_CODE mcp_tool_get_alert_evidence_execute(MCP_CLIENT *mcpc, struct json_object *params, MCP_REQUEST_ID id);

#endif //NETDATA_MCP_TOOLS_ALERT_EVIDENCE_H
