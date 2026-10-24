#!/usr/bin/env python3
"""Guard target-level Browse filtering for restricted v0.5.2 nodes."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "port/open62541/open62541.c").read_text(encoding="utf-8")

start = SOURCE.index("browseReferencTargetCallback(")
end = SOURCE.index("\n}\n\n/* Returns whether the node", start)
callback = SOURCE[start:end]

required = (
    "bc->session != &bc->server->adminSession",
    "accessControl.allowBrowseNode(",
    "&target->head.nodeId, target->head.context",
    "UA_NODESTORE_RELEASE(bc->server, target);",
)
for marker in required:
    if marker not in callback:
        raise AssertionError(f"Browse target filter missing {marker!r}")

policy = callback.index("accessControl.allowBrowseNode(")
description = callback.index("addReferenceDescription(")
if policy >= description:
    raise AssertionError("restricted target is copied before allowBrowseNode policy")

print("v0.5.2 Browse target filter source test: PASS")
